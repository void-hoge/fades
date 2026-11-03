// Black-box, cycle-accurate RTL testbench. No access to internal RAM is used:
// the half-full starting state is built exclusively through ready/valid input.
#include "Vfades_core.h"
#include "verilated.h"
#include "sudoku.hpp"
#include <algorithm>
#include <array>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

using State = VlWide<23>;
// Match core's fixed depth. The prefill goes through its public
// input interface and the assertions below verify the configured capacity.
static constexpr unsigned deque_depth = 1024;
static constexpr unsigned prefill = deque_depth / 2;
static void require(bool value, const std::string& why) {
    if (!value) throw std::runtime_error(why);
}
static unsigned mask_at(const State& s, unsigned cell) {
    const unsigned bit = 9*cell, w = bit/32, shift = bit%32;
    uint32_t v = s[w] >> shift;
    if (shift > 23) v |= s[w+1] << (32-shift);
    return v & 511;
}
static void set_mask(State& s, unsigned cell, unsigned mask) {
    for (unsigned d = 0; d < 9; ++d) {
        const unsigned b = cell*9+d;
        s[b/32] = (s[b/32] & ~(1u << (b%32))) | (((mask >> d)&1u) << (b%32));
    }
}
static State uniform(unsigned mask) {
    State s{};
    for (unsigned i = 0; i < 81; ++i) set_mask(s, i, mask);
    return s;
}
static bool equal(const State& a, const State& b) {
    for (unsigned i = 0; i < 23; ++i) if (a[i] != b[i]) return false;
    return true;
}
struct Problem {
    State input{}, answer{};
    bool solvable = true, exact = true;
    std::string name;
};
static void validate(const Problem& p, const State& answer) {
    std::array<unsigned, 9> row{}, col{}, box{};
    for (unsigned i = 0; i < 81; ++i) {
        unsigned m = mask_at(answer, i), r = i/9, c = i%9, b = r/3*3+c/3;
        require(m && !(m & (m-1)), "Non-singleton output: " + p.name);
        require((m & mask_at(p.input, i)) == m, "Input constraint changed: " + p.name);
        require(!((row[r] | col[c] | box[b]) & m), "Invalid Sudoku: " + p.name);
        row[r] |= m; col[c] |= m; box[b] |= m;
    }
    if (p.exact) require(equal(p.answer, answer), "Reference mismatch: " + p.name);
}
static std::vector<Problem> load_dataset(const char* path, unsigned limit) {
    std::ifstream input(path);
    require(bool(input), "Cannot open dataset");
    std::vector<Problem> problems;
    std::string line;
    while (std::getline(input, line) && (!limit || problems.size() < limit)) {
        if (line.find_first_not_of(" \r\t") == std::string::npos) continue;
        Problem p;
        p.name = "dataset_" + std::to_string(problems.size()+1);
        const auto board = reference::parse(line);
        for (unsigned i = 0; i < 81; ++i) set_mask(p.input, i, board[i]);
        auto solution = reference::solve(board);
        require(solution.solutions == 1, "Reference failed");
        for (unsigned i = 0; i < 81; ++i)
            set_mask(p.answer, i, solution.first[i]);
        validate(p, p.answer);
        problems.push_back(p);
    }
    require(problems.size() >= prefill, "Half-full test needs at least 512 puzzles");
    return problems;
}
struct Stats {
    uint64_t cycles = 0, infer = 0, branch = 0, load = 0, memory = 0, output = 0;
    uint64_t contradiction = 0, skip_events = 0, discarded = 0, finishes = 0;
    uint64_t input_stalls = 0, disabled = 0, idle = 0, pops = 0;
    uint64_t concurrent_push_enqueue = 0, wasted = 0;
    unsigned max_pending = 0, max_queued = 0;
};
static void write_totals(std::ostream& out, const Stats& s) {
    out << "{\"problems\":" << s.finishes << ",\"cycles\":" << s.cycles
        << ",\"inferences\":" << s.infer << ",\"branches\":" << s.branch
        << ",\"loads\":" << s.load << ",\"memory_wait\":" << s.memory
        << ",\"output_wait\":" << s.output << ",\"disabled\":" << s.disabled
        << ",\"idle\":" << s.idle << ",\"contradictions\":" << s.contradiction
        << ",\"branch_pops\":" << s.pops << ",\"skip_events\":" << s.skip_events
        << ",\"discarded\":" << s.discarded << ",\"wasted_inferences\":" << s.wasted
        << ",\"max_pending\":" << s.max_pending << ",\"max_queued\":" << s.max_queued
        << ",\"input_stalls\":" << s.input_stalls
        << ",\"concurrent_push_enqueue\":" << s.concurrent_push_enqueue
        << ",\"prefill\":" << prefill << ",\"deque_depth\":" << deque_depth << '}';
}
static void tick(Vfades_core& dut) {
    dut.clk = 0; dut.eval();
    dut.clk = 1; dut.eval();
    dut.clk = 0; dut.eval();
}
static void reset(Vfades_core& dut) {
    dut.reset = 1; dut.enable = 0; dut.input_valid = 0; dut.result_ready = 0;
    tick(dut); tick(dut); dut.reset = 0; dut.eval();
}
static uint32_t random_word(uint32_t& x) {
    x ^= x << 13; x ^= x >> 17; x ^= x << 5; return x;
}
static Stats run(Vfades_core& dut, const std::vector<Problem>& problems,
                 const std::string& name, bool stress, std::ofstream& csv) {
    reset(dut);
    // Fill exactly half of the deque before enabling inference.
    for (unsigned i = 0; i < prefill; ++i) {
        dut.input_valid = 1; dut.input_candidates = problems[i].input; dut.eval();
        require(dut.input_ready, "Unexpected prefill backpressure");
        tick(dut);
    }
    dut.input_valid = 0;
    require(dut.queued == prefill && !dut.busy, "Incorrect half-full starting state");
    std::cout << name << ": prefill=" << dut.queued << '/' << deque_depth << '\n';
    uint64_t accepted = prefill, returned = 0, finished = 0, previous_finish = 0;
    uint64_t previous_branches = 0, previous_infer = 0, previous_contradictions = 0;
    uint64_t previous_wasted = 0;
    uint32_t rng = 0x7292048;
    bool held = false, held_solved = false;
    State held_result{};
    Stats s;
    // Timeout is deliberately independent of the inference monotonicity bound:
    // DFS may revisit many different states before the first solution.
    while (returned < problems.size() && s.cycles < 100000000) {
        uint32_t random = random_word(rng);
        dut.enable = !stress || ((random & 31) != 0);
        // Long initial output stall fills the one-entry result buffer, then
        // randomized pauses exercise result stability and terminal stalls.
        dut.result_ready = !stress || (s.cycles > 200 && (random & 7) > 1);
        if (!dut.input_valid && accepted < problems.size() &&
            (stress ? ((random & 3) != 0) : dut.queued < prefill)) {
            dut.input_valid = 1;
            dut.input_candidates = problems[accepted].input;
        }
        dut.eval();
        if (held) require(dut.result_valid && held_solved == bool(dut.result_solved) &&
                          equal(held_result, dut.result_candidates), "Result changed under backpressure");
        bool input_taken = dut.input_valid && dut.input_ready;
        bool output_taken = dut.result_valid && dut.result_ready;
        if (output_taken) {
            const Problem& p = problems[returned];
            require(bool(dut.result_solved) == p.solvable, "Wrong solve status: " + p.name);
            if (p.solvable) validate(p, dut.result_candidates);
            ++returned;
        }
        held = dut.result_valid && !dut.result_ready;
        if (held) { held_result = dut.result_candidates; held_solved = dut.result_solved; }
        ++s.cycles;
        s.max_pending = std::max(s.max_pending, unsigned(dut.pending_branches));
        s.max_queued = std::max(s.max_queued, unsigned(dut.queued));
        require(dut.pending_branches <= dut.queued && dut.queued < deque_depth, "Deque invariant failed");
        s.infer += dut.event_infer;
        require(!dut.event_wasted_infer || dut.event_infer, "Wasted cycle is not an inference cycle");
        s.wasted += dut.event_wasted_infer;
        s.branch += dut.event_branch;
        s.load += dut.event_load;
        s.memory += dut.event_memory_wait;
        s.output += dut.event_output_wait;
        s.contradiction += dut.event_contradiction;
        s.skip_events += dut.event_skip;
        s.discarded += dut.event_skip ? dut.pending_branches : 0;
        s.input_stalls += dut.input_valid && !dut.input_ready;
        s.concurrent_push_enqueue += dut.event_branch && input_taken;
        s.disabled += !dut.enable;
        s.pops += dut.event_contradiction && dut.pending_branches != 0;
        unsigned classes = dut.event_infer + dut.event_branch + dut.event_load +
            dut.event_memory_wait + dut.event_output_wait;
        require(classes <= 1, "Overlapping exclusive cycle classes");
        s.idle += dut.enable && !classes;
        if (dut.event_finish) {
            require(finished < problems.size(), "Extra completion");
            if (problems[finished].name == "unsat_requires_search")
                require(s.branch > previous_branches && s.contradiction > previous_contradictions,
                        "Search-unsat case did not exercise exhaustive backtracking");
            if (problems[finished].name == "forced_conflict_metadata")
                require(s.infer - previous_infer == 2 && s.branch == previous_branches,
                        "Delayed deduction-conflict flag lost or misaligned");
            csv << name << ',' << problems[finished].name << ',' << (s.cycles-previous_finish)
                << ',' << (s.infer-previous_infer) << ',' << (s.branch-previous_branches)
                << ',' << (s.contradiction-previous_contradictions)
                << ',' << dut.pending_branches << ',' << (s.wasted-previous_wasted) << '\n';
            previous_finish = s.cycles;
            previous_infer = s.infer;
            previous_branches = s.branch;
            previous_contradictions = s.contradiction;
            previous_wasted = s.wasted;
            ++finished;
        }
        tick(dut);
        if (input_taken) { ++accepted; dut.input_valid = 0; }
        if (s.cycles % 250000 == 0)
            std::cout << name << ": cycles=" << s.cycles << " returned=" << returned << std::endl;
    }
    require(returned == problems.size() && finished == problems.size(),
            "Simulation timeout: scenario=" + name + " finished=" + std::to_string(finished) +
            "/" + std::to_string(problems.size()) + " returned=" + std::to_string(returned) +
            " cycles=" + std::to_string(s.cycles) + "; no complete-run mean reported");
    dut.enable = 1; dut.result_ready = 1; dut.input_valid = 0;
    for (unsigned i = 0; i < 8; ++i) tick(dut);
    require(!dut.busy && !dut.result_valid && dut.queued == 0 && dut.pending_branches == 0,
            "Deque did not drain, or extra result");
    require(s.branch != 0 && s.contradiction != 0 && s.skip_events != 0,
            "Missing DFS / backtracking / skip coverage");
    require(s.cycles == s.infer+s.branch+s.load+s.memory+s.output+s.disabled+s.idle,
            "Cycle accounting mismatch");
    s.finishes = finished;
    require(s.wasted == s.finishes + s.branch + s.pops, "Halt/discard accounting mismatch");
    std::cout << std::fixed << std::setprecision(6)
              << name << ": PASS problems=" << returned << " cycles=" << s.cycles
              << " mean_cycles=" << double(s.cycles)/returned
              << " infer=" << s.infer << " branch=" << s.branch << " load=" << s.load
              << " memory_wait=" << s.memory << " output_wait=" << s.output
              << " disabled=" << s.disabled << " idle=" << s.idle
              << " contradictions=" << s.contradiction << " branch_pops=" << s.pops
              << " skip_events=" << s.skip_events << " discarded=" << s.discarded
              << " max_pending=" << s.max_pending << " max_queued=" << s.max_queued
              << " input_stalls=" << s.input_stalls
              << " concurrent_push_enqueue=" << s.concurrent_push_enqueue
              << " wasted_inferences=" << s.wasted << '\n';
    return s;
}
int main(int argc, char** argv) {
    try {
        require(argc == 3 || argc == 4, "Usage: core_test DATASET RESULTS.csv [LIMIT]");
        auto problems = load_dataset(argv[1], argc == 4 ? std::stoul(argv[3]) : 0);
        std::ofstream csv(argv[2]);
        require(bool(csv), "Cannot open CSV");
        csv << "scenario,problem,cycles,inferences,branches,contradictions,discarded,wasted_inferences\n";
        VerilatedContext context;
        context.threads(1);
        Vfades_core dut{&context};
        auto dataset = run(dut, problems, "dataset", false, csv);
        std::vector<Problem> directed;
        Problem completed;
        completed.input = completed.answer = problems[0].answer;
        completed.name = "completed";
        Problem empty;
        empty.input = uniform(511); empty.exact = false; empty.name = "empty_multiple_solutions";
        Problem zero = completed;
        zero.solvable = false; zero.name = "zero_cell"; set_mask(zero.input, 0, 0);
        Problem duplicate = completed;
        duplicate.solvable = false; duplicate.name = "duplicate_singletons";
        set_mask(duplicate.input, 0, mask_at(duplicate.input, 1));
        Problem hall;
        hall.input = uniform(511); hall.solvable = false; hall.exact = false;
        hall.name = "unsat_requires_search";
        for (unsigned i = 0; i < 9; ++i) set_mask(hall.input, i, i < 3 ? 3 : 508);
        Problem forced;
        forced.input = uniform(511); forced.solvable = false; forced.exact = false;
        forced.name = "forced_conflict_metadata";
        for (unsigned i = 1; i < 9; ++i) {
            set_mask(forced.input, i, 510);
            set_mask(forced.input, i*9, 509);
        }
        // More problems than deque capacity force pointer wrapping as well as push /
        // pop / skip near a half-full queue. Reset reuses non-cleared BRAM.
        for (unsigned i = 0; i < 4096; ++i) {
            switch (i % 64) {
                case 0: directed.push_back(empty); break;
                case 1: directed.push_back(zero); break;
                case 2: directed.push_back(duplicate); break;
                case 3: directed.push_back(hall); break;
                case 4: directed.push_back(forced); break;
                default: directed.push_back(i%2 ? completed : problems[i%problems.size()]);
            }
        }
        // Abort an active search with siblings still in RAM. The next run's
        // reset must invalidate cache, outstanding reads, pointers and results.
        reset(dut);
        dut.input_valid = 1; dut.input_candidates = empty.input; tick(dut);
        dut.input_valid = 0; dut.enable = 1;
        for (unsigned i = 0; i < 1000 && dut.pending_branches < 2; ++i) tick(dut);
        require(dut.busy && dut.pending_branches >= 2, "Active-reset setup failed");
        auto stress = run(dut, directed, "stress", true, csv);
        require(stress.input_stalls && stress.output && stress.disabled && stress.concurrent_push_enqueue,
                "Missing ingress / output / enable stall or dual-write coverage");
        csv.flush();
        require(bool(csv), "Cannot write complete CSV");
        std::ofstream totals(std::string(argv[2]) + ".totals.json");
        require(bool(totals), "Cannot open run totals JSON");
        totals << "{\"dataset\":";
        write_totals(totals, dataset);
        totals << ",\"stress\":";
        write_totals(totals, stress);
        totals << "}\n";
        totals.flush();
        require(bool(totals), "Cannot write run totals JSON");
        dut.final();
        std::cout << "All reference, Sudoku validity, ordering, backpressure, wrap, reset, and accounting checks passed.\n";
    } catch (const std::exception& e) {
        std::cerr << "ERROR: " << e.what() << '\n';
        return 1;
    }
}
