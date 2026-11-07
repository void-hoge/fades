// Explicitly run on KV260: exercise the public fixed-slot class end to end.
#include "fades_stream.hpp"
#include <iostream>
#include <vector>

/** Check all-or-none slots, automatic idle tails, later input and RX backpressure. */
int main() try {
    constexpr unsigned capacity = FADES_DMA_SLOT_BYTES / sizeof(fades::Record);
    fades::SolverStream solver;
    std::vector<fades::Record> inputs(capacity);
    const char* digits = "534678912672195348198342567859761423426853791713924856961537284287419635345286179";
    for (auto& input : inputs) {
        for (unsigned cell = 0; cell < 81; ++cell) {
            input[8 + cell / 2] |= uint8_t((digits[cell] - '0') << (4 * (cell % 2)));
        }
    }
    std::vector<bool> seen(18003);
    auto verify = [&](std::span<const fades::Record> results) {
        for (size_t index = 0; index < results.size(); ++index) {
            const auto& record = results[index];
            uint64_t id = 0;
            for (unsigned byte = 0; byte < 8; ++byte) { id |= uint64_t(record[byte]) << (8 * byte); }
            if (id >= seen.size() || seen[id] || record[8] != 0 ||
                std::memcmp(record.data() + 9, inputs[0].data() + 8, 41) != 0) {
                throw std::runtime_error("Unexpected/duplicate result");
            }
            for (unsigned byte = 50; byte < 64; ++byte) {
                if (record[byte]) { throw std::runtime_error("Reserved result bytes changed"); }
            }
            seen[id] = true;
        }
    };
    if (!solver.try_submit(std::span(inputs).first(9000), 0) ||
        !solver.try_submit(std::span(inputs).first(9000), 9000)) {
        throw std::runtime_error("Initial slots were not accepted in full");
    }
    while (solver.outstanding()) {
        auto output = solver.try_receive();
        if (output) { verify(output->records()); }
        else if (!solver.wait_for_results(10000)) { throw std::runtime_error("Idle tail timeout"); }
    }
    // No finish_input above: an idle tail must be visible and input remains open.
    if (!solver.try_submit(std::span(inputs).first(3), 18000)) {
        throw std::runtime_error("Idle stream did not accept later input");
    }
    while (solver.outstanding()) {
        auto output = solver.try_receive();
        if (output) {
            if (output->size() != 3) { throw std::runtime_error("Incorrect resumed tail size"); }
            verify(output->records());
        } else if (!solver.wait_for_results(10000)) { throw std::runtime_error("Resume timeout"); }
    }
    bool oversized_failed = false;
    try {
        std::vector<fades::Record> oversized(capacity + 1);
        solver.try_submit(oversized);
    } catch (const std::invalid_argument&) {
        oversized_failed = true;
    }
    if (!oversized_failed) { throw std::runtime_error("Oversized input was accepted"); }

    uint64_t submitted = 18003;
    bool saw_busy = false;
    for (unsigned attempt = 0; attempt < 1000; ++attempt) {
        auto before = solver.outstanding();
        if (!solver.try_submit(inputs, submitted)) {
            if (solver.outstanding() != before) { throw std::runtime_error("Busy submit partially accepted input"); }
            saw_busy = true;
            break;
        }
        submitted += capacity;
    }
    if (!saw_busy) { throw std::runtime_error("Did not exercise busy input slots"); }
    seen.resize(submitted);
    solver.finish_input();
    while (solver.outstanding()) {
        auto output = solver.try_receive();
        if (output) { verify(output->records()); }
        else if (!solver.wait(10000)) { throw std::runtime_error("Backpressure drain timeout"); }
    }
    if (!std::all_of(seen.begin(), seen.end(), [](bool value) { return value; })) {
        throw std::runtime_error("Missing copied result");
    }
    const auto stats = solver.statistics();
    for (unsigned index : {0u, 1u, 3u, 4u}) {
        if (stats.counters[index] != submitted) { throw std::runtime_error("Stage counter mismatch"); }
    }
    std::cout << "PASS: SolverStream atomic slots, RX loans, idle tails, resume/backpressure, "
              << submitted << " unique IDs\n";
    return 0;
} catch (const std::exception& error) {
    std::cerr << "FAIL: " << error.what() << '\n';
    return 1;
}
