// Single-threaded QBUF/DQBUF verifier. Every streamed result is checked.
#include "fades_stream.hpp"
#include "sudoku.hpp"
#include <array>
#include <bit>
#include <fstream>
#include <iostream>
#include <vector>

using Record = std::array<uint8_t, 64>;

/** Decode the little-endian request ID from bytes 0..7. */
static uint64_t get_id(const Record& record) {
    uint64_t id = 0;
    for (unsigned byte = 0; byte < 8; ++byte) {
        id |= uint64_t(record[byte]) << (8 * byte);
    }
    return id;
}

/** Replace bytes 0..7 with a little-endian request ID. */
static void set_id(Record& record, uint64_t id) {
    for (unsigned byte = 0; byte < 8; ++byte) {
        record[byte] = id >> (8 * byte);
    }
}

/** OR a 4-bit cell into a zeroed nibble; offset is 8 for input, 9 for output. */
static void set_cell(Record& record, unsigned offset, unsigned cell, unsigned digit) {
    record[offset + cell / 2] |= digit << (4 * (cell % 2));
}

/** Read one 4-bit cell; offset is 8 for input cells, 9 for output cells. */
static unsigned cell(const Record& record, unsigned offset, unsigned index) {
    return (record[offset + index / 2] >> (4 * (index % 2))) & 15;
}

/** Encoded input plus parsed hints and the independent CPU reference solution. */
struct Puzzle {
    Record record{};
    reference::Board input{};
    reference::Board answer{};
};

/** Verify every result in warmup and a finite continuous streaming session. */
int main(int argc, char** argv) try {
    if (argc < 5) {
        throw std::runtime_error("Usage: fades_verify dataset records_per_packet slots rounds [mixed]");
    }
    const unsigned records_per_buffer = std::stoul(argv[2]);
    const unsigned buffer_count = std::stoul(argv[3]);
    const unsigned rounds = std::stoul(argv[4]);
    const bool mixed = argc > 5 && std::string(argv[5]) == "mixed";
    if (!records_per_buffer || records_per_buffer > 16384 || !buffer_count || buffer_count > 8 ||
        !rounds || rounds > 100000) {
        throw std::runtime_error("Invalid transfer dimensions");
    }
    const uint64_t records_per_round = uint64_t(records_per_buffer) * buffer_count;
    if (mixed && records_per_round < 64) {
        throw std::runtime_error("Mixed test requires at least 64 records");
    }
    const auto preparation_begin = fades::Clock::now();
    std::ifstream dataset(argv[1]);
    if (!dataset) {
        throw std::runtime_error("Cannot open dataset");
    }
    std::vector<Puzzle> puzzles;
    std::string line;
    while (std::getline(dataset, line)) {
        if (line.find_first_not_of(" \r\t") == std::string::npos) {
            continue;
        }
        Puzzle puzzle;
        puzzle.input = reference::parse(line);
        auto reference_result = reference::solve(puzzle.input);
        if (reference_result.solutions != 1) {
            throw std::runtime_error("CPU reference found no solution");
        }
        puzzle.answer = reference_result.first;
        for (unsigned index = 0; index < 81; ++index) {
            if (reference::single(puzzle.input[index])) {
                set_cell(puzzle.record, 8, index, std::countr_zero(puzzle.input[index]) + 1);
            }
        }
        puzzles.push_back(puzzle);
    }
    if (dataset.bad()) {
        throw std::runtime_error("Dataset read failed");
    }
    if (puzzles.empty()) {
        throw std::runtime_error("Empty dataset");
    }
    const auto oracle_done = fades::Clock::now();
    fades::Device device;
    std::vector<Record> staging(records_per_buffer);

    // Run total records with IDs id_base + ordinal, checking every RX record.
    // A logical round changes mixed-input selection, not the streaming session.
    auto execute = [&](uint64_t total, uint64_t id_base) {
        std::vector<bool> seen(total);
        uint64_t unsolved_records = 0;
        uint64_t invalid_records = 0;
        auto expected_status = [&](uint64_t ordinal) -> unsigned {
            return mixed && ordinal % records_per_round >= records_per_round - 64 ? (ordinal % 2 ? 2 : 1) : 0;
        };
        auto write_input_buffer = [&](uint8_t* destination, unsigned count, uint64_t first_record) {
            for (unsigned index = 0; index < count; ++index) {
                uint64_t ordinal = first_record + index;
                auto& record = staging[index];
                record = puzzles[ordinal % puzzles.size()].record;
                if (expected_status(ordinal)) {
                    record = {};
                    if (expected_status(ordinal) == 2) {
                        set_cell(record, 8, ordinal % 81, 10 + (ordinal / 2) % 6);
                    } else {
                        set_cell(record, 8, 0, 1);
                        set_cell(record, 8, 1, 1);
                    }
                }
                set_id(record, id_base + ordinal);
            }
            std::memcpy(destination, staging.data(), size_t(count) * 64);
        };
        auto read_output_buffer = [&](unsigned buffer_index, unsigned count) {
            for (unsigned index = 0; index < count; ++index) {
                Record record;
                std::memcpy(record.data(), device.buffer_data(FADES_QUEUE_RX, buffer_index) + size_t(index) * 64, 64);
                uint64_t id = get_id(record);
                if (id < id_base || id - id_base >= total) {
                    throw std::runtime_error("Unknown ID");
                }
                uint64_t ordinal = id - id_base;
                if (seen[ordinal]) {
                    throw std::runtime_error("Duplicate ID");
                }
                seen[ordinal] = true;
                if (record[8] != expected_status(ordinal)) {
                    throw std::runtime_error("Unexpected result status");
                }
                if (!expected_status(ordinal)) {
                    reference::Board answer{};
                    for (unsigned position = 0; position < 81; ++position) {
                        unsigned digit = cell(record, 9, position);
                        if (digit < 1 || digit > 9) {
                            throw std::runtime_error("Bad digit");
                        }
                        answer[position] = 1u << (digit - 1);
                    }
                    const auto& puzzle = puzzles[ordinal % puzzles.size()];
                    if (!reference::valid_solution(puzzle.input, answer) || answer != puzzle.answer) {
                        throw std::runtime_error("Solution mismatch");
                    }
                } else {
                    if (expected_status(ordinal) == 1) {
                        ++unsolved_records;
                    } else {
                        ++invalid_records;
                    }
                    for (unsigned position = 0; position < 81; ++position) {
                        if (cell(record, 9, position) != 0) {
                            throw std::runtime_error("Nonzero error cells");
                        }
                    }
                }
                if ((record[49] & 0xf0) != 0) {
                    throw std::runtime_error("Reserved bits changed");
                }
                for (unsigned byte = 50; byte < 64; ++byte) {
                    if (record[byte] != 0) {
                        throw std::runtime_error("Reserved bytes changed");
                    }
                }
            }
        };
        auto result = fades::run_streaming_session(device, records_per_buffer, buffer_count,
                                                    total, 0, write_input_buffer, read_output_buffer);
        std::cout << "Verified " << total << " unique IDs: solved=" << total - unsolved_records - invalid_records
                  << ", unsolved=" << unsolved_records << ", invalid=" << invalid_records << '\n';
        return result;
    };
    execute(records_per_round, 0x1234567800000000ULL);
    auto result = execute(records_per_round * rounds, 0xcafe987600000000ULL);
    std::cout << "Continuous stream: " << result.completed_records / result.elapsed_seconds / 1e6
              << " Mproblem/s, " << result.elapsed_seconds << " s, " << result.completed_packets
              << " packets; final drain " << result.drain_seconds << " s\n"
              << "Oracle preparation (excluded): " << fades::seconds(preparation_begin, oracle_done) << " s\n"
              << "All streamed results validated inside elapsed time; initial DDR fill excluded.\n"
              << "Session PL counters:";
    for (auto counter : result.solver_stats.counters) {
        std::cout << ' ' << counter;
    }
    std::cout << "\nPASS\n";
    return 0;
} catch (const std::exception& error) {
    std::cerr << "FAIL: " << error.what() << '\n';
    return 1;
}
