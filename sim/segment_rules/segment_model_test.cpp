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
            unsigned rows = 0;
            for (unsigned c = 0; c < 81; ++c)
                if (board[c] != expected.next[c]) rows |= 1u << (c/9);
            require(dut.row_changed == rows, "row change mask differs");
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
            if (SEGMENT_RULES_MODE >= 3) {
                auto old = segment_model::step(board,segment_model::Mode::triples);
                auto next = segment_model::step(board,segment_model::Mode(SEGMENT_RULES_MODE));
                for (unsigned c = 0; c < 81; ++c) {
                    require(!(next.next[c] & ~old.next[c]), "pair mode lost a triple elimination");
                    if (trial % 2) require(next.next[c] & truth[c], "pair mode removed a solution digit");
                }
            }
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
                        for (auto mode : {segment_model::Mode::contradiction,segment_model::Mode::triples,
                                          segment_model::Mode::pairs_triples,segment_model::Mode::bivalue_pairs}) {
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
        unsigned pair_cases = 0, support_cases = 0;
        if (SEGMENT_RULES_MODE >= 3) {
            // Every fixed segment, every source pair (including nonadjacent
            // cells), every pair of digits. Triple mode is already stalled;
            // pair mode must remove exactly from the shared line/box peers.
            for (unsigned o = 0; o < 2; ++o)
                for (unsigned line = 0; line < 9; ++line)
                    for (unsigned part = 0; part < 3; ++part)
                        for (unsigned excluded = 0; excluded < 3; ++excluded)
                            for (unsigned d = 0; d < 9; ++d)
                                for (unsigned e = d+1; e < 9; ++e) {
                                    auto cells = segment_model::segment(o,line,part);
                                    auto a = cells[(excluded+1)%3], b = cells[(excluded+2)%3];
                                    unsigned digits = (1u << d) | (1u << e);
                                    reference::Board board;
                                    board.fill(511); board[a] = board[b] = digits;
                                    compare(board);
                                    auto old = segment_model::step(board,segment_model::Mode::triples);
                                    require(old.next == board, "strict-strength witness is not stalled in mode 2");
                                    auto next = unpack(dut.candidates_out);
                                    unsigned box = (a/27)*3+(a%9)/3;
                                    for (unsigned c = 0; c < 81; ++c) {
                                        bool shares = (o == 0 ? c/9 == line : c%9 == line) ||
                                            (c/27)*3+(c%9)/3 == box;
                                        auto expected = c != a && c != b && shares ? board[c] & ~digits : board[c];
                                        require(next[c] == expected, "pair source exclusion or target mismatch");
                                    }
                                    ++pair_cases;
                                }
            // Independent local oracle: enumerate distinct digit assignments,
            // rather than Hall subsets, for every 4-digit domain combination.
            // Check both orientations, empty domains and Hall contradictions.
            for (unsigned o = 0; o < 2; ++o) {
                auto cells = segment_model::segment(o,4,1);
                for (unsigned pattern = 0; pattern < 4096; ++pattern) {
                    reference::Board board;
                    board.fill(511);
                    for (unsigned k = 0; k < 3; ++k) board[cells[k]] = (pattern >> (4*k)) & 15;
                    std::array<unsigned,3> supported{};
                    bool possible = false;
                    for (unsigned a = 0; a < 4; ++a)
                        for (unsigned b = 0; b < 4; ++b)
                            for (unsigned c = 0; c < 4; ++c)
                                if (a != b && a != c && b != c &&
                                    (board[cells[0]] & (1u << a)) &&
                                    (board[cells[1]] & (1u << b)) &&
                                    (board[cells[2]] & (1u << c))) {
                                    possible = true;
                                    supported[0] |= 1u << a; supported[1] |= 1u << b; supported[2] |= 1u << c;
                                }
                    compare(board);
                    require(bool(dut.contradiction) == !possible, "local feasibility differs from assignment oracle");
                    if (possible) {
                        auto next = unpack(dut.candidates_out);
                        if (SEGMENT_RULES_MODE == 4) {
                            // The inexpensive variant omits the one-update
                            // singleton+bivalue shortcut, not the closure.
                            auto state = board;
                            for (unsigned round = 0; round < 5; ++round) {
                                compare(state);
                                next = unpack(dut.candidates_out);
                                if (next == state) break;
                                state = next;
                            }
                        }
                        for (unsigned k = 0; k < 3; ++k)
                            require(next[cells[k]] == supported[k], "local support is not complete");
                    }
                    ++support_cases;
                }
            }
        }
        std::cout << "PASS mode=" << SEGMENT_RULES_MODE << ": 8000 random RTL/model comparisons; " << preservation_cases
                  << " solution-preserving segment cases; all 54 removal/contradiction locations; "
                  << pair_cases << " strict-strength pair cases; " << support_cases << " assignment-oracle cases\n";
    } catch (const std::exception& e) {
        std::cerr << "FAIL: " << e.what() << '\n';
        return 1;
    }
}
