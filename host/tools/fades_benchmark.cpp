// Duration-driven PS stress test. Coherent buffers are CPU-accessed only
// outside RUN. Timed rounds check DMA completion and coherent PL snapshots;
// full content validation is deliberately outside the timed window.
#include "fades_ioctl.h"
#include "sudoku.hpp"
#include <algorithm>
#include <array>
#include <bit>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstring>
#include <cstdlib>
#include <fcntl.h>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <stdexcept>
#include <string>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>
#include <vector>

using Clock = std::chrono::steady_clock;
using Record = std::array<uint8_t, 64>;
static void require(bool value, const std::string& message) {
    if (!value) throw std::runtime_error(message);
}
static void device_ioctl(int fd, unsigned long command, void* arg, const char* name) {
    if (ioctl(fd, command, arg) < 0)
        throw std::runtime_error(std::string(name) + ": " + std::strerror(errno));
}
static double seconds(Clock::time_point a, Clock::time_point b) {
    return std::chrono::duration<double>(b-a).count();
}
static double parse_duration(const char* text) {
    char* end = nullptr;
    errno = 0;
    double result = std::strtod(text, &end);
    require(end != text && *end == '\0' && errno != ERANGE &&
            std::isfinite(result) && result >= 0.001 && result <= 86400,
            "Duration must be a finite number from 0.001 to 86400 seconds");
    return result;
}
struct Puzzle {
    Record input{};
    reference::Board board{};
    bool solvable = false;
};
static std::vector<Puzzle> load(const char* path) {
    std::ifstream file(path);
    require(bool(file), std::string("Cannot open puzzle file: ") + path);
    std::vector<Puzzle> puzzles;
    std::string line;
    unsigned line_number = 0;
    while (std::getline(file, line)) {
        ++line_number;
        Puzzle p;
        if (line.find_first_not_of(" \r\t") == std::string::npos) continue;
        try {
            p.board = reference::parse(line);
        } catch (const std::runtime_error& e) {
            throw std::runtime_error("Line " + std::to_string(line_number) + ": " + e.what());
        }
        for (unsigned c = 0; c < 81; ++c)
            if (reference::single(p.board[c]))
                p.input[8+c/2] |= uint8_t((std::countr_zero(p.board[c])+1) << (4*(c%2)));
        puzzles.push_back(p);
    }
    require(!file.bad(), "Puzzle file read failed");
    require(!puzzles.empty(), "Puzzle file is empty");
    std::cout << "Loaded " << puzzles.size() << " puzzles; preparing independent CPU references..." << std::endl;
    for (auto& p : puzzles) p.solvable = reference::solve(p.board).solutions != 0;
    return puzzles;
}
struct Device {
    int fd = -1;
    uint8_t* tx = nullptr;
    uint8_t* rx = nullptr;
    fades_dma_info info{};
    Device() = default;
    Device(const Device&) = delete;
    Device& operator=(const Device&) = delete;
    ~Device() {
        if (rx) munmap(rx, info.map_bytes);
        if (tx) munmap(tx, info.map_bytes);
        if (fd >= 0) close(fd);
    }
    void open_device() {
        fd = open("/dev/fades", O_RDWR | O_CLOEXEC);
        if (fd < 0) throw std::runtime_error(std::string("Open solver: ") + std::strerror(errno));
        device_ioctl(fd, FADES_DMA_INFO, &info, "INFO");
        require(info.abi_version == FADES_DMA_ABI_VERSION && info.slots == FADES_DMA_SLOTS &&
                info.slot_bytes == FADES_DMA_SLOT_BYTES &&
                info.map_bytes == uint64_t(info.slots)*info.slot_bytes, "Unsupported DMA ABI");
        void* mapping = mmap(nullptr, info.map_bytes, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        require(mapping != MAP_FAILED, "TX mmap failed");
        tx = static_cast<uint8_t*>(mapping);
        mapping = mmap(nullptr, info.map_bytes, PROT_READ | PROT_WRITE, MAP_SHARED, fd, info.map_bytes);
        require(mapping != MAP_FAILED, "RX mmap failed");
        rx = static_cast<uint8_t*>(mapping);
    }
};
static uint64_t record_id(const Record& r) {
    uint64_t result = 0;
    for (unsigned b = 0; b < 8; ++b) result |= uint64_t(r[b]) << (8*b);
    return result;
}
struct Batch {
    Device& dev;
    const std::vector<Puzzle>& puzzles;
    const unsigned per, count;
    size_t offset = 0;
    uint64_t tag = 0;
    explicit Batch(Device& d, const std::vector<Puzzle>& p)
        : dev(d), puzzles(p), per(d.info.slot_bytes/64), count(per*d.info.slots) {}
    const Puzzle& puzzle(unsigned index) const { return puzzles[(offset+index)%puzzles.size()]; }
    void prepare(size_t first, uint64_t salt) {
        offset = first; tag = salt;
        for (unsigned i = 0; i < count; ++i) {
            Record record = puzzle(i).input;
            const uint64_t id = tag+i;
            for (unsigned b = 0; b < 8; ++b) record[b] = uint8_t(id >> (8*b));
            std::memcpy(dev.tx+size_t(i)*64, record.data(), record.size());
        }
    }
    uint64_t solved_per_round() const {
        uint64_t result = 0;
        for (unsigned i = 0; i < count; ++i) result += puzzle(i).solvable;
        return result;
    }
    void verify(const char* phase) const {
        std::vector<bool> seen(count);
        uint64_t solved = 0, unsolved = 0;
        for (unsigned i = 0; i < count; ++i) {
            Record r;
            std::memcpy(r.data(), dev.rx+size_t(i)*64, r.size());
            uint64_t id = record_id(r);
            require(id >= tag && id-tag < count, "Unknown or stale result ID");
            unsigned index = unsigned(id-tag);
            require(!seen[index], "Duplicate result ID");
            seen[index] = true;
            const auto& p = puzzle(index);
            require(r[8] == (p.solvable ? 0 : 1), "Unexpected result status");
            if (p.solvable) {
                reference::Board answer{};
                for (unsigned c = 0; c < 81; ++c) {
                    unsigned digit = (r[9+c/2] >> (4*(c%2))) & 15;
                    require(digit >= 1 && digit <= 9, "Invalid solved digit");
                    answer[c] = reference::Mask(1u << (digit-1));
                }
                require(reference::valid_solution(p.board, answer), "Invalid Sudoku solution or changed hint");
                ++solved;
            } else {
                for (unsigned b = 9; b < 50; ++b) require(r[b] == 0, "Nonzero unsolved payload");
                ++unsolved;
            }
            require((r[49] & 0xf0) == 0, "Nonzero reserved nibble");
            for (unsigned b = 50; b < 64; ++b) require(r[b] == 0, "Nonzero reserved bytes");
        }
        std::cout << phase << " verification PASS: " << count << " IDs, solved=" << solved
                  << ", unsolved=" << unsolved << std::endl;
    }
};
struct Totals {
    uint64_t rounds = 0, processed = 0, solvable = 0, within = 0, after = 0, kernel_ns = 0;
    uint64_t input_stall = 0, queue_peak = 0;
    std::array<uint64_t, 6> dispatched{}, collected{}, max_inflight{};
    void accumulate(const fades_solver_stats& s, unsigned count) {
        for (unsigned i : {0u, 1u, 3u, 4u}) require(s.counters[i] == count, "PL stage count mismatch");
        uint64_t sent = 0, got = 0;
        for (unsigned g = 0; g < 6; ++g) {
            sent += s.counters[6+g]; got += s.counters[12+g];
            require(s.counters[38+g] <= 2, "PL inflight limit exceeded");
            dispatched[g] += s.counters[6+g]; collected[g] += s.counters[12+g];
            max_inflight[g] = std::max(max_inflight[g], uint64_t(s.counters[38+g]));
        }
        require(sent == count && got == count && s.counters[31] != 0, "PL per-core/work count mismatch");
        require(s.counters[30] <= 66, "PL FIFO occupancy exceeds capacity");
        if (s.counters[2]) require(s.counters[30] >= 64, "PL FIFO occupancy measurement failed");
        queue_peak = std::max(queue_peak, uint64_t(s.counters[30]));
        input_stall += s.counters[2];
    }
};
int main(int argc, char** argv) try {
    require(argc == 3, "Usage: fades_benchmark PUZZLE_FILE SECONDS");
    const double duration = parse_duration(argv[2]);
    const auto prep_begin = Clock::now();
    auto puzzles = load(argv[1]);
    Device dev;
    dev.open_device();
    Batch batch(dev, puzzles);
    fades_dma_run run{};
    run.bytes = dev.info.slot_bytes; run.slots = dev.info.slots;
    run.rounds = 1; run.timeout_ms = 60000;
    batch.prepare(0, 0x1234567800000000ULL);
    device_ioctl(dev.fd, FADES_DMA_RUN, &run, "Warmup RUN");
    batch.verify("Warmup");
    batch.prepare(0, 0xcafe987600000000ULL);
    const bool refill = puzzles.size() > batch.count;
    std::cout << "Batch: " << batch.count << " records, " << dev.info.slots << " x "
              << run.bytes << " bytes; source=" << (refill ? "rotating windows" : "prepared DDR replay")
              << "; requested=" << duration << " s\n"
              << "Preparation and warmup: " << seconds(prep_begin, Clock::now())
              << " s (excluded). Starting timed stress..." << std::endl;
    Totals totals;
    uint64_t batch_solved = batch.solved_per_round();
    double refill_seconds = 0, last_run_seconds = 0;
    const auto start = Clock::now();
    const auto deadline = start + std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(duration));
    while (Clock::now() < deadline) {
        if (refill && totals.rounds) {
            const auto copy_start = Clock::now();
            const auto old_offset = batch.offset;
            const auto old_tag = batch.tag;
            batch.prepare((batch.offset+batch.count)%puzzles.size(), batch.tag+batch.count);
            batch_solved = batch.solved_per_round();
            refill_seconds += seconds(copy_start, Clock::now());
            if (Clock::now() >= deadline) {
                // RX still holds the last completed window. Keep its identity
                // for final validation; do not start DMA after the deadline.
                batch.offset = old_offset; batch.tag = old_tag;
                break;
            }
        }
        const auto run_start = Clock::now();
        device_ioctl(dev.fd, FADES_DMA_RUN, &run, "Timed RUN");
        const auto finish = Clock::now();
        last_run_seconds = seconds(run_start, finish);
        require(run.bytes_per_direction == uint64_t(batch.count)*64, "DMA byte count mismatch");
        ++totals.rounds;
        totals.processed += batch.count;
        totals.solvable += batch_solved;
        totals.kernel_ns += run.elapsed_ns;
        if (finish <= deadline) totals.within += batch.count;
        else totals.after += batch.count;
        fades_solver_stats snapshot{};
        device_ioctl(dev.fd, FADES_SOLVER_STATS, &snapshot, "STATS");
        totals.accumulate(snapshot, batch.count);
    }
    const auto end = Clock::now();
    require(totals.rounds != 0, "No timed batch completed");
    const auto validation_start = Clock::now();
    batch.verify("Final");
    const double elapsed = seconds(start, end);
    std::cout << std::fixed << std::setprecision(6)
              << "Requested seconds: " << duration << '\n'
              << "Actual timed seconds (including final batch drain and snapshots): " << elapsed << '\n'
              << "Completed requests: " << totals.processed << '\n'
              << "Solvable-input completions (CPU reference classification): " << totals.solvable << '\n'
              << "Unsolvable-input completions (CPU reference classification): " << totals.processed-totals.solvable << '\n'
              << "Throughput: " << double(totals.processed)/elapsed << " problem/s ("
              << double(totals.processed)/elapsed/1e6 << " Mproblem/s)\n"
              << "Whole-batch completions within requested window: " << totals.within << '\n'
              << "Final batch completions after deadline: " << totals.after << '\n'
              << "Deadline overrun seconds: " << std::max(0.0, elapsed-duration) << '\n'
              << "Last RUN seconds: " << last_run_seconds << '\n'
              << "Completed rounds: " << totals.rounds << '\n'
              << "Summed kernel RUN seconds: " << double(totals.kernel_ns)/1e9 << '\n'
              << "Timed input refill seconds: " << refill_seconds << '\n'
              << "Final content validation seconds (excluded): " << seconds(validation_start, Clock::now()) << '\n'
              << "Errors: 0\nFIFO write-domain peak: " << totals.queue_peak << '\n'
              << "DMA input stall cycles (sum): " << totals.input_stall << '\n';
    for (unsigned g = 0; g < 6; ++g)
        std::cout << "Core " << g << ": dispatched=" << totals.dispatched[g]
                  << ", collected=" << totals.collected[g] << ", max_inflight=" << totals.max_inflight[g] << '\n';
    std::cout << "Timed rounds verify DMA completion and PL counts; content checks cover warmup and final batch.\n"
              << "Counts are repeated requests, not distinct puzzles. The final batch is never aborted.\n";
    if (refill) std::cout << "File exceeds DMA capacity: window preparation/copy is included in timed elapsed.\n";
    return 0;
} catch (const std::exception& e) {
    std::cerr << "FAIL: " << e.what() << '\n';
    return 1;
}
