#pragma once

#include "sudoku.hpp"
#include <algorithm>
#include <array>
#include <bit>
#include <cstdint>
#include <stdexcept>

// Simulation-only, snapshot-based specification of main's cell engine.
// Extra rules also read the ORIGINAL board, never another rule's successor.
namespace segment_model {
using reference::Board;
using reference::Mask;
constexpr Mask all = reference::all;
enum class Mode { baseline, contradiction, triples, pairs_triples, bivalue_pairs };
struct Step {
    Board next{};
    bool solved = false, contradiction = false, forced_conflict = false;
    bool segment_conflict = false;
    unsigned tight_segments = 0, useful_segments = 0, extra_removed = 0;
    unsigned tight_pairs = 0, pair_extra_removed = 0;
};
constexpr std::array<unsigned,3> segment(unsigned orientation, unsigned line, unsigned part) {
    std::array<unsigned,3> cells{};
    for (unsigned k = 0; k < 3; ++k)
        cells[k] = orientation == 0 ? line*9+part*3+k : (part*3+k)*9+line;
    return cells;
}
inline Step step(const Board& board, Mode mode) {
    Step out;
    out.next = board;
    std::array<Mask,27> used{}, unique{};
    std::array<Mask,81> fixed{}, removed{}, extra{}, pair_extra{};
    bool filled = true;
    for (unsigned c = 0; c < 81; ++c) {
        if (!board[c]) out.contradiction = true;
        if (reference::single(board[c])) fixed[c] = board[c];
        else filled = false;
    }
    for (unsigned u = 0; u < 27; ++u) {
        Mask seen = 0, multiple = 0;
        for (auto c : reference::units[u]) {
            multiple |= seen & board[c];
            seen |= board[c];
            if (used[u] & fixed[c]) out.contradiction = true;
            used[u] |= fixed[c];
        }
        if (seen != all) out.contradiction = true;
        unique[u] = seen & Mask(~multiple);
    }
    out.solved = filled && !out.contradiction;
    // Enumerate digit positions, independently of RTL's segment-mask network.
    for (unsigned u = 0; u < 27; ++u) {
        for (unsigned d = 0; d < 9; ++d) {
            unsigned positions = 0;
            for (unsigned p = 0; p < 9; ++p)
                if (board[reference::units[u][p]] & (1u << d)) positions |= 1u << p;
            if (!positions) continue;
            auto remove_outside = [&](unsigned target) {
                for (auto c : reference::units[target]) {
                    bool inside = u < 9 ? c/9 == u : u < 18 ? c%9 == u-9 :
                        (c/27)*3+(c%9)/3 == u-18;
                    if (!inside) removed[c] |= 1u << d;
                }
            };
            for (unsigned g = 0; g < 3; ++g) {
                if (!(positions & ~(7u << (3*g)))) {
                    if (u < 9) remove_outside(18+(u/3)*3+g);
                    else if (u < 18) remove_outside(18+g*3+(u-9)/3);
                    else remove_outside(((u-18)/3)*3+g);
                }
                if (u >= 18 && !(positions & ~(0x49u << g)))
                    remove_outside(9+((u-18)%3)*3+g);
            }
        }
    }
    for (unsigned c = 0; c < 81; ++c) {
        unsigned r = c/9, col = c%9, box = r/3*3+col/3;
        Mask peers = (used[r] | used[9+col] | used[18+box]) & Mask(~fixed[c]);
        Mask forced = board[c] & (unique[r] | unique[9+col] | unique[18+box]);
        out.forced_conflict |= forced && !reference::single(forced);
        out.next[c] &= Mask(~(peers | removed[c]));
        if (forced) out.next[c] &= forced;
    }
    if (mode == Mode::baseline) return out;
    for (unsigned orientation = 0; orientation < 2; ++orientation) {
        for (unsigned line = 0; line < 9; ++line) {
            for (unsigned part = 0; part < 3; ++part) {
                auto cells = segment(orientation,line,part);
                unsigned box = (cells[0]/27)*3+(cells[0]%9)/3;
                if (mode == Mode::pairs_triples || mode == Mode::bivalue_pairs) {
                    // Enumerate cell subsets, independently of RTL's three
                    // masks indexed by the excluded local cell.
                    for (unsigned subset = 1; subset < 8; ++subset) {
                        if (std::popcount(subset) != 2) continue;
                        Mask digits = 0;
                        for (unsigned k = 0; k < 3; ++k)
                            if (subset & (1u << k)) digits |= board[cells[k]];
                        // <2 is already covered by empty/fixed-digit checks.
                        if (std::popcount(digits) != 2) continue;
                        if (mode == Mode::bivalue_pairs) {
                            unsigned a = 3, b = 3;
                            for (unsigned k = 0; k < 3; ++k)
                                if (subset & (1u << k)) { if (a == 3) a = k; else b = k; }
                            if (board[cells[a]] != digits || board[cells[b]] != digits) continue;
                        }
                        ++out.tight_pairs;
                        for (unsigned c = 0; c < 81; ++c) {
                            bool inside = false;
                            for (unsigned k = 0; k < 3; ++k)
                                inside |= (subset & (1u << k)) && c == cells[k];
                            bool same_line = orientation == 0 ? c/9 == line : c%9 == line;
                            bool same_box = (c/27)*3+(c%9)/3 == box;
                            if (!inside && (same_line || same_box)) pair_extra[c] |= digits;
                        }
                    }
                }
                Mask digits = board[cells[0]] | board[cells[1]] | board[cells[2]];
                unsigned count = std::popcount(digits);
                out.segment_conflict |= count < 3;
                if (count != 3) continue;
                ++out.tight_segments;
                if (mode == Mode::contradiction) continue;
                bool useful = false;
                for (unsigned c = 0; c < 81; ++c) {
                    if (std::find(cells.begin(), cells.end(), c) != cells.end()) continue;
                    bool same_line = orientation == 0 ? c/9 == line : c%9 == line;
                    bool same_box = (c/27)*3+(c%9)/3 == box;
                    if (!same_line && !same_box) continue;
                    extra[c] |= digits;
                    useful |= bool(out.next[c] & digits);
                }
                out.useful_segments += useful;
            }
        }
    }
    for (unsigned c = 0; c < 81; ++c) {
        // Count only eliminations additional to the baseline step. Overlapping
        // segments removing the same cell/digit are counted once here.
        out.pair_extra_removed += std::popcount(Mask(out.next[c] & pair_extra[c] & ~extra[c]));
        out.extra_removed += std::popcount(Mask(out.next[c] & (extra[c] | pair_extra[c])));
        out.next[c] &= Mask(~(extra[c] | pair_extra[c]));
    }
    return out;
}
struct Stats {
    std::uint64_t infer = 0, branches = 0, contradictions = 0, skip_events = 0;
    std::uint64_t tight_segments = 0, useful_segments = 0, useful_cycles = 0;
    std::uint64_t extra_removed = 0, segment_conflict_cycles = 0;
    std::uint64_t tight_pairs = 0, pair_extra_removed = 0, pair_useful_cycles = 0;
    std::uint64_t pair_at_triple_stalls = 0, pair_bits_at_triple_stalls = 0;
    unsigned max_pending = 0;
};
inline std::array<Board,2> branch(const Board& board) {
    unsigned chosen = 81;
    for (unsigned c = 0; c < 81; ++c) {
        unsigned count = std::popcount(board[c]);
        if (count < 2) continue;
        if (chosen == 81) chosen = c;
        if (count == 2) { chosen = c; break; }
    }
    if (chosen == 81) throw std::runtime_error("stalled board has no branch");
    std::array<Board,2> children{board,board};
    Mask remaining = board[chosen], half = 0;
    for (unsigned n = std::popcount(remaining)/2; n; --n) {
        Mask bit = Mask(remaining & -remaining);
        half |= bit; remaining ^= bit;
    }
    children[0][chosen] = half;
    children[1][chosen] = remaining;
    return children;
}
inline bool search(Board& board, Mode mode, Stats& stats, unsigned pending = 0) {
    if (pending > 324) throw std::runtime_error("cell search reserve exceeded");
    stats.max_pending = std::max(stats.max_pending, pending);
    bool last_changed = true, last_forced_conflict = false;
    for (;;) {
        ++stats.infer;
        auto next = step(board, mode);
        if (next.contradiction || next.segment_conflict || last_forced_conflict) {
            ++stats.contradictions;
            stats.segment_conflict_cycles += next.segment_conflict &&
                !next.contradiction && !last_forced_conflict;
            return false;
        }
        if (next.solved) { stats.skip_events += pending != 0; return true; }
        if (!last_changed) {
            if (mode == Mode::triples) {
                // Shadow probe only: do not change the mode-2 search or cycle
                // estimate. This distinguishes new deductions from shortcuts
                // that triples would obtain after more propagation rounds.
                auto stronger = step(board,Mode::pairs_triples);
                if (stronger.next != board) {
                    ++stats.pair_at_triple_stalls;
                    stats.pair_bits_at_triple_stalls += stronger.pair_extra_removed;
                }
            }
            break;
        }
        last_changed = board != next.next;
        last_forced_conflict = next.forced_conflict;
        board = next.next;
        stats.tight_segments += next.tight_segments;
        stats.useful_segments += next.useful_segments;
        stats.useful_cycles += next.extra_removed != 0;
        stats.extra_removed += next.extra_removed;
        stats.tight_pairs += next.tight_pairs;
        stats.pair_extra_removed += next.pair_extra_removed;
        stats.pair_useful_cycles += next.pair_extra_removed != 0;
    }
    auto children = branch(board);
    ++stats.branches;
    if (search(children[0],mode,stats,pending+1)) { board = children[0]; return true; }
    if (search(children[1],mode,stats,pending)) { board = children[1]; return true; }
    return false;
}
} // namespace segment_model
