#include "Vtriad_test_top.h"
#include "verilated.h"
#include "sudoku.hpp"
#include "triad.hpp"
#include <fstream>
#include <iostream>
#include <random>
#include <string>

static void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
using Packed = VlWide<49>;
static Packed pack(const triad::State& s) {
    Packed p{};
    for (unsigned i = 0; i < 171; ++i) {
        auto m = i < 81 ? s.cells[i] : i < 135 ? s.negative[i-81] : s.configs[i-135];
        for (unsigned d = 0; d < 9; ++d) if (m & (1u << d))
            p[(9*i+d)/32] |= 1u << ((9*i+d)%32);
    }
    return p;
}
static triad::State unpack(const Packed& p) {
    triad::State s;
    for (unsigned i = 0; i < 171; ++i) {
        triad::Mask m = 0;
        for (unsigned d = 0; d < 9; ++d)
            m |= ((p[(9*i+d)/32] >> ((9*i+d)%32)) & 1u) << d;
        (i < 81 ? s.cells[i] : i < 135 ? s.negative[i-81] : s.configs[i-135]) = m;
    }
    return s;
}
static void compare(Vtriad_test_top& dut, const triad::State& s, bool phase) {
    dut.state_in = pack(s);
    dut.band_phase = phase;
    dut.eval();
    auto expected = triad::step(s, phase);
    require(unpack(dut.state_out) == expected.state, "phase state mismatch");
    require(bool(dut.contradiction) == expected.contradiction, "contradiction mismatch");
    auto b = triad::branch(s);
    require(bool(dut.branch_valid) == b.valid, "branch validity mismatch");
    require(unpack(dut.child_a) == b.a && unpack(dut.child_b) == b.b, "branch mismatch");
    for (unsigned cycle = 0; cycle < 2; ++cycle) {
        dut.clk = 0; dut.eval();
        dut.clk = 1; dut.eval();
        dut.clk = 0; dut.eval();
    }
    require(bool(dut.pipelined_valid) == b.valid && unpack(dut.pipelined_a) == b.a &&
            unpack(dut.pipelined_b) == b.b, "pipelined selector mismatch after stable input");
    if (b.valid) {
        unsigned changes = 0;
        for (unsigned i = 0; i < 36; ++i) {
            require((b.a.configs[i] | b.b.configs[i]) == s.configs[i], "branch coverage");
            auto selected = s.configs[i] ^ (b.a.configs[i] & b.b.configs[i]);
            changes += std::popcount(unsigned(selected));
        }
        require(changes >= 2, "branch made no progress");
        require(b.a != s && b.b != s, "non-strict branch");
    }
    auto out = unpack(dut.state_out);
    auto in = pack(s), next = pack(out);
    for (unsigned i = 0; i < 49; ++i)
        require(!(next[i] & ~in[i]), "candidate added");
}
static triad::State solution_state() {
    triad::State s;
    for (unsigned r = 0; r < 9; ++r)
        for (unsigned c = 0; c < 9; ++c)
            s.cells[9*r+c] = 1u << ((r*3+r/3+c)%9);
    for (unsigned box = 0; box < 9; ++box)
        for (unsigned t = 0; t < 6; ++t) {
            triad::Mask present = 0;
            for (unsigned k = 0; k < 3; ++k)
                present |= s.cells[triad::cell(box, triad::local_cell(t/3,t%3,k))];
            s.negative[6*box+t] = triad::all ^ present;
        }
    for (unsigned o = 0; o < 2; ++o)
        for (unsigned b = 0; b < 3; ++b)
            for (unsigned f = 0; f < 6; ++f) {
                triad::Mask supported = triad::all;
                for (unsigned p = 0; p < 3; ++p) {
                    unsigned box = o == 0 ? 3*b+p : 3*p+b;
                    supported &= triad::all ^ s.negative[6*box+3*o+triad::permutations[f][p]];
                }
                s.configs[6*(3*o+b)+f] = supported;
            }
    return s;
}
int main(int argc, char** argv) {
    try {
        require(argc == 2, "usage: triad_test DATASET");
        VerilatedContext context;
        Vtriad_test_top dut{&context};
        std::mt19937 rng(0x1539512);
        for (unsigned mask = 1; mask < 64; ++mask) {
            triad::State s;
            s.cells.fill(511); s.negative.fill(511);
            for (unsigned band = 0; band < 6; ++band) s.configs[6*band] = 511;
            for (unsigned f = 0; f < 6; ++f)
                s.configs[f] = (f == 0 ? 510 : 0) | ((mask >> f) & 1u);
            compare(dut, s, false);
            auto children = triad::branch(s);
            if (children.valid) {
                unsigned ac = 0, bc = 0;
                for (unsigned f = 0; f < 6; ++f) {
                    ac += children.a.configs[f] & 1u;
                    bc += children.b.configs[f] & 1u;
                }
                require(ac == unsigned(std::popcount(mask))/2 && ac+bc == unsigned(std::popcount(mask)),
                        "unbalanced configuration split");
            }
        }
        auto solution = solution_state();
        for (unsigned trial = 0; trial < 6000; ++trial) {
            triad::State s = solution;
            for (auto& m : s.cells) m |= rng() & 511;
            for (auto& m : s.negative) m |= rng() & 511;
            for (auto& m : s.configs) m |= rng() & 511;
            for (bool phase : {false, true}) {
                compare(dut, s, phase);
                require(!dut.contradiction, "valid solution declared contradictory");
                auto out = pack(unpack(dut.state_out)), truth = pack(solution);
                for (unsigned i = 0; i < 49; ++i)
                    require((out[i] & truth[i]) == truth[i], "legal solution eliminated");
            }
            // Also exercise contradictory and unrelated candidate combinations.
            for (auto& m : s.cells) m = rng() & 511;
            for (auto& m : s.negative) m = rng() & 511;
            for (auto& m : s.configs) m = rng() & 511;
            compare(dut, s, false);
            compare(dut, s, true);
        }
        // A single missing configuration must survive packing and both phases;
        // it must not be reconstructed from projected cell candidates.
        auto excluded = triad::initial(std::array<triad::Mask,81>{});
        excluded.cells.fill(511);
        excluded.configs[0] &= ~1u;
        require(triad::close(excluded), "configuration exclusion contradiction");
        require(!(excluded.configs[0] & 1), "configuration exclusion lost");
        std::ifstream input(argv[1]);
        require(bool(input), "cannot open dataset");
        std::string line;
        unsigned puzzles = 0;
        while (puzzles < 512 && std::getline(input, line)) {
            auto board = reference::parse(line);
            std::array<triad::Mask,81> cells;
            for (unsigned i = 0; i < 81; ++i) cells[i] = board[i];
            auto s = triad::initial(cells);
            require(triad::solve(s), "scalar model failed to solve");
            auto oracle = reference::solve(board);
            for (unsigned i = 0; i < 81; ++i)
                require(s.cells[i] == oracle.first[i], "scalar solution mismatch");
            ++puzzles;
        }
        require(puzzles == 512, "insufficient dataset");
        std::cout << "PASS: 24000 RTL/model phases, legal solution preservation, balanced branches, "
                  << puzzles << " independently checked scalar solutions\n";
    } catch (const std::exception& e) {
        std::cerr << "FAIL: " << e.what() << '\n';
        return 1;
    }
}
