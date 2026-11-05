// Streaming benchmark: identical input states for propagation-only and full DFS.
// CSV datasets may supply puzzle,solution; plain 81-character lines also work.
#include "verilated.h"
#ifdef CORE_BENCH
#include "Vfades_core.h"
using Dut = Vfades_core;
#else
#include "Vdeduction_engine.h"
using Dut = Vdeduction_engine;
#endif
#include <algorithm>
#include <array>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

using State = VlWide<23>;
static unsigned mask_at(const State& s, unsigned i) {
    unsigned bit = i*9, shift = bit%32;
    uint32_t v = s[bit/32] >> shift;
    if (shift > 23) v |= s[bit/32+1] << (32-shift);
    return v & 511;
}
static State encode(const std::string& p) {
    State s{};
    for (unsigned i=0; i<81; ++i) {
        char c = p[i];
        if (c != '.' && (c < '0' || c > '9')) throw std::runtime_error("Invalid cell");
        unsigned m = (c == '.' || c == '0') ? 511 : 1u << (c-'1');
        for (unsigned d=0; d<9; ++d) s[(i*9+d)/32] |= ((m>>d)&1u) << ((i*9+d)%32);
    }
    return s;
}
static bool equal(const State& a, const State& b) {
    for (unsigned i=0; i<23; ++i) if (a[i] != b[i]) return false;
    return true;
}
static void validate(const State& input, const State& answer) {
    std::array<unsigned,9> rows{}, cols{}, boxes{};
    for (unsigned i=0; i<81; ++i) {
        unsigned m=mask_at(answer,i), r=i/9, c=i%9, b=r/3*3+c/3;
        if (!m || (m&(m-1)) || (m & mask_at(input,i)) != m ||
            ((rows[r]|cols[c]|boxes[b]) & m)) throw std::runtime_error("Invalid solution");
        rows[r]|=m; cols[c]|=m; boxes[b]|=m;
    }
}
#ifdef CORE_BENCH
static void tick(Dut& d) { d.clk=0; d.eval(); d.clk=1; d.eval(); d.clk=0; d.eval(); }
#endif
int main(int argc, char** argv) {
    try {
        if (argc < 3 || argc > 4) throw std::runtime_error("Usage: benchmark DATASET OUTPUT.csv [LIMIT]");
        std::ifstream in(argv[1]); std::ofstream out(argv[2]);
        if (!in || !out) throw std::runtime_error("Cannot open input/output");
        uint64_t limit=argc==4 ? std::stoull(argv[3]) : 0;
        VerilatedContext context; context.threads(1); Dut d{&context};
#ifdef CORE_BENCH
        d.reset=1; d.enable=0; d.input_valid=0; d.result_ready=1; tick(d); tick(d); d.reset=0;
#endif
        out << "problem,cycles,inferences,branches,contradictions,memory_wait,solved,stalled\n";
        uint64_t n=0, total=0, solved_count=0, stalled_count=0;
        std::string line;
        while ((!limit || n<limit) && std::getline(in,line)) {
            if (!line.empty() && line.back()=='\r') line.pop_back();
            if (line.empty()) continue;
            auto comma=line.find(',');
            std::string puzzle=line.substr(0,comma);
            if (puzzle=="puzzle" || puzzle=="quizzes") continue;
            if (puzzle.size()!=81) throw std::runtime_error("Expected 81 cells at problem " + std::to_string(n+1));
            State input=encode(puzzle), answer{};
            uint64_t cycles=0, infer=0, branch=0, contradiction=0, memory=0;
            bool solved=false, stalled=false;
#ifdef CORE_BENCH
            // Empty-queue ingress and initial load are excluded. The timed window
            // starts in RUN and ends on event_finish, with no supply/output stalls.
            d.enable=0; d.input_candidates=input; d.input_valid=1; d.eval();
            if (!d.input_ready) throw std::runtime_error("Core not drained");
            tick(d); d.input_valid=0; d.enable=1; tick(d);
            while (cycles < 100000000) {
                d.eval(); ++cycles; infer+=d.event_infer; branch+=d.event_branch;
                contradiction+=d.event_contradiction; memory+=d.event_memory_wait;
                bool finish=d.event_finish;
                tick(d);
                if (finish) { solved=d.result_solved; answer=d.result_candidates; break; }
            }
            if (cycles==100000000) throw std::runtime_error("Per-problem timeout; no complete mean");
            if (!solved) throw std::runtime_error("Unexpected unsatisfiable puzzle");
            tick(d); // drain registered result, outside timing
#else
            State current=input;
            while (cycles < 730) {
                d.candidates_in=current; d.eval(); ++cycles; ++infer;
                if (d.contradiction || d.forced_conflict) { ++contradiction; break; }
                if (d.solved) { solved=true; answer=current; break; }
                if (equal(current,d.candidates_out)) { stalled=true; break; }
                current=d.candidates_out;
            }
            if (!solved && !stalled && !contradiction) throw std::runtime_error("Propagation timeout");
#endif
            if (solved) validate(input,answer);
            // When supplied, check the known solution remains in every domain.
            // This also checks unfinished propagation rather than only solved cases.
            if (comma!=std::string::npos) {
                std::string solution=line.substr(comma+1);
                if (solution.size()!=81) throw std::runtime_error("Expected solution column");
                State expected=encode(solution);
                validate(input,expected);
#ifndef CORE_BENCH
                for (unsigned i=0;i<81;++i)
                    if ((mask_at(expected,i)&mask_at(current,i))!=mask_at(expected,i))
                        throw std::runtime_error("Propagation removed reference solution");
#endif
                if (solved && !equal(expected,answer)) throw std::runtime_error("Reference mismatch");
            }
            if (contradiction && !solved && !stalled) {
#ifndef CORE_BENCH
                throw std::runtime_error("Unexpected propagation contradiction");
#endif
            }
            ++n; total+=cycles; solved_count+=solved; stalled_count+=stalled;
            out << n << ',' << cycles << ',' << infer << ',' << branch << ',' << contradiction
                << ',' << memory << ',' << solved << ',' << stalled << '\n';
        }
        if (!n) throw std::runtime_error("Empty dataset");
        std::cout << "problems=" << n << " cycles=" << total << " mean_cycles=" << double(total)/n
                  << " solved=" << solved_count << " stalled=" << stalled_count << '\n';
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
