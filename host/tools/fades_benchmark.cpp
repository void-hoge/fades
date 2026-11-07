// Duration-driven single-threaded streaming benchmark. Initial preparation,
// CPU reference and warmup/final full-content checks are outside timing.
#include "fades_stream.hpp"
#include "sudoku.hpp"
#include <array>
#include <bit>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <unordered_set>
#include <vector>

using Record = std::array<uint8_t, 64>;

/** Parsed input and independent CPU solvability classification for validation. */
struct Puzzle {
    reference::Board board{};
    bool solvable = false;
};

/** Parse a finite duration in [0.001, 86400] seconds, or throw runtime_error. */
static double parse_duration(const char* text) {
    char* end = nullptr;
    errno = 0;
    double duration = std::strtod(text, &end);
    if (end == text || *end != '\0' || errno == ERANGE || !std::isfinite(duration) ||
        duration < 0.001 || duration > 86400) {
        throw std::runtime_error("Duration must be a finite number from 0.001 to 86400 seconds");
    }
    return duration;
}

/** Decode the little-endian request ID in bytes 0..7 of a 64-byte record. */
static uint64_t record_id(const Record& record) {
    uint64_t id = 0;
    for (unsigned byte = 0; byte < 8; ++byte) {
        id |= uint64_t(record[byte]) << (8 * byte);
    }
    return id;
}

/** Run warmup, a duration-limited stream, and final resident-buffer validation. */
int main(int argc, char** argv) try {
    if (argc != 3) {
        throw std::runtime_error("Usage: fades_benchmark PUZZLE_FILE SECONDS");
    }
    const double duration = parse_duration(argv[2]);
    std::ifstream file(argv[1]);
    if (!file) {
        throw std::runtime_error("Cannot open puzzle file");
    }
    std::vector<Puzzle> puzzles;
    std::vector<Record> corpus;
    std::string line;
    while (std::getline(file, line)) {
        if (line.find_first_not_of(" \r\t") == std::string::npos) {
            continue;
        }
        Puzzle puzzle;
        puzzle.board = reference::parse(line);
        Record record{};
        for (unsigned cell = 0; cell < 81; ++cell) {
            if (reference::single(puzzle.board[cell])) {
                record[8 + cell / 2] |= uint8_t((std::countr_zero(puzzle.board[cell]) + 1) << (4 * (cell % 2)));
            }
        }
        puzzles.push_back(puzzle);
        corpus.push_back(record);
    }
    if (file.bad()) {
        throw std::runtime_error("Puzzle file read failed");
    }
    if (puzzles.empty()) {
        throw std::runtime_error("Puzzle file is empty");
    }
    std::cout << "Loaded " << puzzles.size() << " puzzles; preparing independent CPU references..." << std::endl;
    for (auto& puzzle : puzzles) {
        puzzle.solvable = reference::solve(puzzle.board).solutions != 0;
    }
    fades::Device device;
    constexpr unsigned records_per_buffer = 10000;
    const unsigned buffer_count = device.info().slots;
    uint64_t id_base = 0;

    // Populate exactly count input records, cycling the corpus and assigning
    // IDs id_base + first_record + offset. Invoked with CPU-owned TX memory.
    auto write_input_buffer = [&](uint8_t* target, unsigned count, uint64_t first_record) {
        unsigned copied = 0;
        while (copied < count) {
            size_t source_offset = (first_record + copied) % corpus.size();
            unsigned chunk = unsigned(std::min<size_t>(count - copied, corpus.size() - source_offset));
            fades::copy_records_with_ids(target + size_t(copied) * 64, corpus[source_offset].data(),
                                         chunk, id_base + first_record + copied);
            copied += chunk;
        }
    };

    // Check one result against its corpus input and add its ID to the phase's
    // duplicate detector. Called only outside the measured benchmark interval.
    auto verify_record = [&](const Record& record, uint64_t total, std::unordered_set<uint64_t>& seen) {
        uint64_t id = record_id(record);
        if (id < id_base || id - id_base >= total) {
            throw std::runtime_error("Unknown or stale result ID");
        }
        if (!seen.insert(id).second) {
            throw std::runtime_error("Duplicate result ID");
        }
        const auto& puzzle = puzzles[(id - id_base) % puzzles.size()];
        if (record[8] != (puzzle.solvable ? 0 : 1)) {
            throw std::runtime_error("Unexpected result status");
        }
        if (puzzle.solvable) {
            reference::Board answer{};
            for (unsigned cell = 0; cell < 81; ++cell) {
                unsigned digit = (record[9 + cell / 2] >> (4 * (cell % 2))) & 15;
                if (digit < 1 || digit > 9) {
                    throw std::runtime_error("Invalid solved digit");
                }
                answer[cell] = reference::Mask(1u << (digit - 1));
            }
            if (!reference::valid_solution(puzzle.board, answer)) {
                throw std::runtime_error("Invalid Sudoku solution or changed hint");
            }
        } else {
            for (unsigned byte = 9; byte < 50; ++byte) {
                if (record[byte] != 0) {
                    throw std::runtime_error("Nonzero unsolved payload");
                }
            }
        }
        if ((record[49] & 0xf0) != 0) {
            throw std::runtime_error("Nonzero reserved nibble");
        }
        for (unsigned byte = 50; byte < 64; ++byte) {
            if (record[byte] != 0) {
                throw std::runtime_error("Nonzero reserved bytes");
            }
        }
    };

    id_base = 0x1234567800000000ULL;
    std::vector<Record> warmup_records;
    auto warmup = fades::run_streaming_session(
        device, records_per_buffer, buffer_count, uint64_t(records_per_buffer) * buffer_count,
        0, write_input_buffer, [&](unsigned index, unsigned count) {
            // Preserve warmup results before this RX buffer is recycled.
            auto previous_size = warmup_records.size();
            warmup_records.resize(previous_size + count);
            std::memcpy(warmup_records.data() + previous_size,
                        device.buffer_data(FADES_QUEUE_RX, index), size_t(count) * 64);
        });
    std::unordered_set<uint64_t> seen;
    for (const auto& record : warmup_records) {
        verify_record(record, warmup.completed_records, seen);
    }
    std::cout << "Warmup verification PASS: " << warmup.completed_records << " IDs\n"
              << "Streaming: " << buffer_count << " slots x " << records_per_buffer
              << " records; requested=" << duration << " s\n"
              << "Refill/copy and unique ID generation are included; initial DDR fill is excluded." << std::endl;

    id_base = 0xcafe987600000000ULL;
    std::array<unsigned, FADES_DMA_SLOTS> last_buffer_lengths{};
    auto result = fades::run_streaming_session(
        device, records_per_buffer, buffer_count, std::numeric_limits<uint64_t>::max(),
        duration, write_input_buffer, [&](unsigned index, unsigned count) {
            // Track only the final resident length; avoid timed content scans.
            last_buffer_lengths[index] = count;
        });
    seen.clear();
    for (unsigned buffer = 0; buffer < buffer_count; ++buffer) {
        for (unsigned index = 0; index < last_buffer_lengths[buffer]; ++index) {
            Record record;
            std::memcpy(record.data(), device.buffer_data(FADES_QUEUE_RX, buffer) + size_t(index) * 64, 64);
            verify_record(record, result.submitted_records, seen);
        }
    }
    std::cout << "Final resident-buffer verification PASS: " << seen.size() << " IDs\n"
              << std::fixed << std::setprecision(6)
              << "Requested seconds: " << duration << '\n'
              << "Actual timed seconds (including final drain): " << result.elapsed_seconds << '\n'
              << "Completed requests: " << result.completed_records << '\n'
              << "Throughput: " << result.completed_records / result.elapsed_seconds << " problem/s ("
              << result.completed_records / result.elapsed_seconds / 1e6 << " Mproblem/s)\n"
              << "Packet completions observed within requested window: " << result.records_observed_before_deadline << '\n'
              << "Observed-window throughput: " << result.records_observed_before_deadline / duration / 1e6 << " Mproblem/s\n"
              << "Final drain seconds: " << result.drain_seconds << '\n'
              << "Deadline overrun seconds: " << std::max(0.0, result.elapsed_seconds - duration) << '\n'
              << "Completed packets: " << result.completed_packets << '\n'
              << "Timed input refill seconds: " << result.refill_seconds << '\n'
              << "FIFO write-domain peak: " << result.solver_stats.counters[30] << '\n'
              << "DMA input stall cycles: " << result.solver_stats.counters[2] << '\n'
              << "Input FIFO empty cycles: " << result.solver_stats.counters[5] << '\n';
    for (unsigned core = 0; core < 6; ++core) {
        std::cout << "Core " << core << ": dispatched=" << result.solver_stats.counters[6 + core]
                  << ", collected=" << result.solver_stats.counters[12 + core]
                  << ", input_wait=" << result.solver_stats.counters[32 + core]
                  << ", output_wait=" << result.solver_stats.counters[24 + core] << '\n';
    }
    std::cout << "Errors: 0\nCounts include repeated puzzles. Full content checks cover warmup and final resident buffers;\n"
              << "the timed stream checks DMA lengths and final cumulative PL counts.\n";
    return 0;
} catch (const std::exception& error) {
    std::cerr << "FAIL: " << error.what() << '\n';
    return 1;
}
