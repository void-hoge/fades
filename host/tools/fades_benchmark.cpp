// Fixed-slot benchmark. The loop here owns the deadline and measurements;
// SolverStream only submits/copies whole slots and waits for device progress.
// Every timed output byte is read into a checksum before releasing its RX loan.
#include "fades_stream.hpp"
#include "sudoku.hpp"
#include <array>
#include <bit>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <unordered_set>
#include <vector>

using Record = std::array<uint8_t, 64>;
using Clock = std::chrono::steady_clock;

/** Convert a monotonic elapsed interval to seconds. */
static double seconds(Clock::time_point begin, Clock::time_point end) {
    return std::chrono::duration<double>(end - begin).count();
}

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

/** Read every result byte into an observable checksum, with no destination copy. */
static uint64_t consume_results(std::span<const fades::Record> records) {
#if defined(__aarch64__)
    uint64x2_t first = vdupq_n_u64(0);
    uint64x2_t second = vdupq_n_u64(0);
    uint64x2_t third = vdupq_n_u64(0);
    uint64x2_t fourth = vdupq_n_u64(0);
    for (size_t index = 0; index < records.size(); ++index) {
        if (index + 8 < records.size()) { __builtin_prefetch(records[index + 8].data(), 0, 3); }
        auto record = vld1q_u8_x4(records[index].data());
        first = vaddq_u64(first, vreinterpretq_u64_u8(record.val[0]));
        second = vaddq_u64(second, vreinterpretq_u64_u8(record.val[1]));
        third = vaddq_u64(third, vreinterpretq_u64_u8(record.val[2]));
        fourth = vaddq_u64(fourth, vreinterpretq_u64_u8(record.val[3]));
    }
    auto sum = vaddq_u64(vaddq_u64(first, second), vaddq_u64(third, fourth));
    return vgetq_lane_u64(sum, 0) + vgetq_lane_u64(sum, 1);
#else
    uint64_t sum = 0;
    for (const auto& record : records) {
        for (unsigned offset = 0; offset < 64; offset += 8) {
            uint64_t word;
            std::memcpy(&word, record.data() + offset, sizeof(word));
            sum += word;
        }
    }
    return sum;
#endif
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
    constexpr unsigned records_per_buffer = FADES_DMA_SLOT_BYTES / sizeof(Record);
    constexpr unsigned buffer_count = FADES_DMA_SLOTS;
    uint64_t id_base = 0;

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

    std::unordered_set<uint64_t> seen;
    const uint64_t warmup_count = uint64_t(records_per_buffer) * buffer_count;
    // Independent correctness sessions before/after timing, with all answers
    // checked directly in their borrowed slots. No benchmark loop is hidden.
    auto verify_session = [&](uint64_t phase_id_base, const char* phase) {
        id_base = phase_id_base;
        seen.clear();
        fades::SolverStream warmup;
        uint64_t submitted = 0;
        while (submitted < warmup_count || warmup.outstanding()) {
            auto output = warmup.try_receive();
            size_t received = output ? output->size() : 0;
            if (output) {
                for (const auto& record : output->records()) { verify_record(record, warmup_count, seen); }
                output->release();
            }
            bool accepted = false;
            if (submitted < warmup_count) {
                size_t offset = submitted % corpus.size();
                size_t count = std::min<uint64_t>({records_per_buffer, corpus.size() - offset,
                                                  warmup_count - submitted});
                auto inputs = std::span(corpus).subspan(offset, count);
                accepted = warmup.try_submit(inputs, id_base + submitted);
                if (accepted) { submitted += count; }
                if (submitted == warmup_count) { warmup.finish_input(); }
            }
            if (!received && !accepted && !warmup.wait(60000)) {
                throw std::runtime_error("Warmup progress timeout");
            }
        }
        if (seen.size() != warmup_count) { throw std::runtime_error("Missing verification result"); }
        std::cout << phase << " verification PASS: " << warmup_count << " IDs\n";
    };
    verify_session(0x1234567800000000ULL, "Warmup");
    std::cout << "Streaming: " << buffer_count << " slots x " << records_per_buffer
              << " records; requested=" << duration << " s\n"
              << "Input copy/ID updates, reading ALL 64 result bytes, and RX recycling are included; no output copy." << std::endl;

    id_base = 0xcafe987600000000ULL;
    {
        fades::SolverStream solver;
        uint64_t checksum = 0;
        uint64_t submitted = 0;
        uint64_t received = 0;
        uint64_t packets = 0;
        uint64_t observed_before_deadline = 0;
        double submission_seconds = 0;
        const auto begin = Clock::now();
        const auto deadline = begin + std::chrono::duration_cast<Clock::duration>(
            std::chrono::duration<double>(duration));

        while (Clock::now() < deadline) {
            auto output = solver.try_receive();
            size_t completed = output ? output->size() : 0;
            if (output) {
                checksum += consume_results(output->records());
                output->release();
                received += completed;
                ++packets;
                if (Clock::now() <= deadline) { observed_before_deadline += completed; }
            }
            if (Clock::now() >= deadline) { break; }
            size_t offset = submitted % corpus.size();
            size_t count = std::min<size_t>(records_per_buffer, corpus.size() - offset);
            auto inputs = std::span(corpus).subspan(offset, count);
            const auto submission_begin = Clock::now();
            bool accepted = solver.try_submit(inputs, id_base + submitted);
            submission_seconds += seconds(submission_begin, Clock::now());
            if (accepted) { submitted += count; }
            if (!completed && !accepted) { solver.wait_until(deadline); }
        }

        solver.finish_input();
        const auto drain_begin = Clock::now();
        while (solver.outstanding()) {
            auto output = solver.try_receive();
            size_t completed = output ? output->size() : 0;
            if (output) {
                checksum += consume_results(output->records());
                output->release();
                received += completed;
                ++packets;
            } else if (!solver.wait(60000)) {
                throw std::runtime_error("Final drain timeout");
            }
        }
        const auto end = Clock::now();
        const double elapsed = seconds(begin, end);
        const auto statistics = solver.statistics();
        if (received != submitted) { throw std::runtime_error("Missing results"); }
        for (unsigned index : {0u, 1u, 3u, 4u}) {
            if (statistics.counters[index] != submitted) { throw std::runtime_error("PL stage count mismatch"); }
        }
        uint64_t dispatched = 0;
        uint64_t collected = 0;
        for (unsigned core = 0; core < 6; ++core) {
            dispatched += statistics.counters[6 + core];
            collected += statistics.counters[12 + core];
            if (statistics.counters[38 + core] > 2) { throw std::runtime_error("PL inflight limit exceeded"); }
        }
        if (dispatched != submitted || collected != submitted) { throw std::runtime_error("PL core count mismatch"); }

        std::cout << std::fixed << std::setprecision(6)
              << "Requested seconds: " << duration << '\n'
              << "Actual timed seconds (including final drain): " << elapsed << '\n'
              << "Completed requests: " << received << '\n'
              << "Throughput: " << received / elapsed << " problem/s (" << received / elapsed / 1e6 << " Mproblem/s)\n"
              << "Packet completions observed within requested window: " << observed_before_deadline << '\n'
              << "Observed-window throughput: " << observed_before_deadline / duration / 1e6 << " Mproblem/s\n"
              << "Final drain seconds: " << seconds(drain_begin, end) << '\n'
              << "Deadline overrun seconds: " << std::max(0.0, elapsed - duration) << '\n'
              << "Completed packets: " << packets << '\n'
              << "Timed input submission seconds: " << submission_seconds << '\n'
              << "All-result checksum: " << checksum << '\n'
              << "FIFO write-domain peak: " << statistics.counters[30] << '\n'
              << "DMA input stall cycles: " << statistics.counters[2] << '\n'
              << "Input FIFO empty cycles: " << statistics.counters[5] << '\n';
        for (unsigned core = 0; core < 6; ++core) {
            std::cout << "Core " << core << ": dispatched=" << statistics.counters[6 + core]
                      << ", collected=" << statistics.counters[12 + core]
                      << ", input_wait=" << statistics.counters[32 + core]
                      << ", output_wait=" << statistics.counters[24 + core] << '\n';
        }
    }
    verify_session(0x5678123400000000ULL, "Post-run");
    std::cout << "Errors: 0\nCounts include repeated puzzles. Full content checks cover independent pre/post sessions;\n"
              << "the timed stream reads all result bytes and checks DMA lengths/final PL counts.\n";
    return 0;
} catch (const std::exception& error) {
    std::cerr << "FAIL: " << error.what() << '\n';
    return 1;
}
