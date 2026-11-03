#include "Vdeduction_engine.h"
#include "verilated.h"
#include "segment_model.hpp"
#include <iostream>
#include <random>

#ifndef SEGMENT_RULES_MODE
#define SEGMENT_RULES_MODE 0
#endif

static void require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}
static VlWide<23> pack(const reference::Board& board) {
    VlWide<23> packed{};
    for (unsigned c = 0; c < 81; ++c)
        for (unsigned d = 0; d < 9; ++d)
            if (board[c] & (1u << d)) packed[(9*c+d)/32] |= 1u << ((9*c+d)%32);
    return packed;
}
static reference::Board unpack(const VlWide<23>& packed) {
    reference::Board board{};
    for (unsigned c = 0; c < 81; ++c)
        for (unsigned d = 0; d < 9; ++d)
            board[c] |= ((packed[(9*c+d)/32] >> ((9*c+d)%32)) & 1u) << d;
    return board;
}
int main() {
    try {
        VerilatedContext context;
        Vdeduction_engine dut{&context};
        std::mt19937 rng(0x543729);
        auto compare = [&](const reference::Board& board) {
            auto expected = segment_model::step(board,segment_model::Mode(SEGMENT_RULES_MODE));
            dut.candidates_in = pack(board); dut.eval();
            require(unpack(dut.candidates_out) == expected.next, "successor differs from snapshot model");
            require(bool(dut.solved) == (expected.solved && !expected.segment_conflict), "solved differs");
            require(bool(dut.contradiction) == (expected.contradiction || expected.segment_conflict),
                    "contradiction differs");
            require(bool(dut.forced_conflict) == expected.forced_conflict, "forced conflict differs");
        };
        reference::Board truth;
        for (unsigned r = 0; r < 9; ++r)
            for (unsigned c = 0; c < 9; ++c) truth[9*r+c] = 1u << ((r*3+r/3+c)%9);
        for (unsigned trial = 0; trial < 8000; ++trial) {
            reference::Board board;
            for (unsigned c = 0; c < 81; ++c) {
                board[c] = rng() & 511;
                if (trial % 2) board[c] |= truth[c];
            }
            compare(board);
        }
        // Each fixed triple has three distinct digits in this known solution.
        // Enumerate every nonempty candidate subset containing each true digit:
        // 4^3 patterns for every one of the 54 segment locations.
        unsigned preservation_cases = 0;
        for (unsigned o = 0; o < 2; ++o) {
            for (unsigned line = 0; line < 9; ++line) {
                for (unsigned part = 0; part < 3; ++part) {
                    auto cells = segment_model::segment(o,line,part);
                    reference::Mask digits = truth[cells[0]] | truth[cells[1]] | truth[cells[2]];
                    for (unsigned pattern = 0; pattern < 64; ++pattern) {
                        reference::Board board;
                        board.fill(511);
                        for (unsigned k = 0; k < 3; ++k) {
                            auto others = reference::Mask(digits & ~truth[cells[k]]);
                            auto first = reference::Mask(others & -others);
                            auto second = reference::Mask(others ^ first);
                            board[cells[k]] = truth[cells[k]] |
                                ((pattern & (1u << (2*k))) ? first : 0) |
                                ((pattern & (1u << (2*k+1))) ? second : 0);
                        }
                        require(reference::valid_solution(board,truth), "invalid solution witness");
                        compare(board);
                        for (auto mode : {segment_model::Mode::contradiction,segment_model::Mode::triples}) {
                            auto result = segment_model::step(board,mode);
                            require(!result.contradiction && !result.segment_conflict && !result.forced_conflict,
                                    "legal assignment declared contradictory");
                            for (unsigned c = 0; c < 81; ++c)
                                require((result.next[c] & truth[c]) == truth[c], "solution candidate eliminated");
                        }
                        ++preservation_cases;
                    }
                    reference::Board board;
                    board.fill(511);
                    for (auto c : cells) board[c] = digits & ~truth[cells[0]];
                    compare(board);
                    require(segment_model::step(board,segment_model::Mode::contradiction).segment_conflict,
                            "three cells with two digits were not rejected");
                    board.fill(511);
                    for (auto c : cells) board[c] = digits;
                    compare(board);
                    auto base = segment_model::step(board,segment_model::Mode::baseline);
                    auto extra = segment_model::step(board,segment_model::Mode::triples);
                    require(base.extra_removed == 0 && extra.extra_removed > 0, "triple elimination not exercised");
                    unsigned box = (cells[0]/27)*3+(cells[0]%9)/3;
                    for (unsigned c = 0; c < 81; ++c) {
                        bool inside = std::find(cells.begin(),cells.end(),c) != cells.end();
                        bool shares = (o == 0 ? c/9 == line : c%9 == line) || (c/27)*3+(c%9)/3 == box;
                        auto expected = reference::Mask(!inside && shares ? 511 & ~digits : board[c]);
                        require(extra.next[c] == expected, "triple removal target mismatch");
                    }
                }
            }
        }
        std::cout << "PASS mode=" << SEGMENT_RULES_MODE << ": 8000 random RTL/model comparisons; " << preservation_cases
                  << " solution-preserving segment cases; all 54 removal/contradiction locations\n";
    } catch (const std::exception& e) {
        std::cerr << "FAIL: " << e.what() << '\n';
        return 1;
    }
}
