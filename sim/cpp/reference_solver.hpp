#pragma once
#include <array>
#include <bit>
#include <cstdint>
#include <sstream>
#include <stdexcept>
#include <string>

// Independent CPU oracle. Sequential propagation, rather than RTL's parallel
// step semantics. Every recursive call owns its board (no undo bookkeeping).
namespace reference {
using Mask = uint16_t;
using Board = std::array<Mask, 81>;
constexpr Mask all = 0x1ff;
inline bool single(Mask m) { return m && !(m & (m - 1)); }
inline constexpr auto units = [] {
    std::array<std::array<unsigned, 9>, 27> result{};
    for (unsigned u = 0; u < 27; ++u)
        for (unsigned p = 0; p < 9; ++p)
            result[u][p] = u < 9 ? 9*u+p : u < 18 ? 9*p+u-9 :
                ((u-18)/3)*27 + ((u-18)%3)*3 + (p/3)*9 + p%3;
    return result;
}();

inline Board parse(const std::string& line) {
    Board board{};
    std::istringstream input(line);
    std::string token;
    for (auto& m : board) {
        if (!(input >> token) || !(token == "-" ||
            (token.size() == 1 && token[0] >= '1' && token[0] <= '9')))
            throw std::runtime_error("Expected 81 whitespace-separated digits or '-'");
        m = token == "-" ? all : Mask(1u << (token[0]-'1'));
    }
    if (input >> token) throw std::runtime_error("Extra puzzle token");
    return board;
}

inline bool valid_solution(const Board& input, const Board& answer) {
    for (unsigned i = 0; i < 81; ++i)
        if (!single(answer[i]) || (answer[i] & input[i]) != answer[i]) return false;
    for (const auto& unit : units) {
        Mask used = 0;
        for (auto i : unit) {
            if (used & answer[i]) return false;
            used |= answer[i];
        }
        if (used != all) return false;
    }
    return true;
}

// Return false on contradiction. Stop at a solved board or a fixed point.
inline bool propagate(Board& board) {
    for (;;) {
        const Board before = board;
        std::array<Mask, 27> used{};
        for (unsigned u = 0; u < 27; ++u) {
            for (auto i : units[u]) {
                Mask m = board[i];
                if (!m || (m & ~all)) return false;
                if (single(m)) {
                    if (used[u] & m) return false;
                    used[u] |= m;
                }
            }
        }
        // Naked singles: eliminate all already fixed peer labels together.
        for (unsigned i = 0; i < 81; ++i) {
            if (single(board[i])) continue;
            unsigned r = i/9, c = i%9, b = r/3*3+c/3;
            board[i] &= Mask(~(used[r] | used[9+c] | used[18+b]));
            if (!board[i]) return false;
        }
        for (unsigned u = 0; u < 27; ++u) {
            // Presence/uniqueness scans are independent of the RTL tree.
            Mask seen = 0, multiple = 0;
            for (auto i : units[u]) {
                multiple |= seen & board[i];
                seen |= board[i];
            }
            if (seen != all) return false;
            Mask unique = seen & Mask(~multiple);
            for (auto i : units[u]) {
                Mask forced = board[i] & unique;
                if (forced) {
                    if (!single(forced)) return false;
                    board[i] = forced;
                }
            }
            // Locked candidates: digit positions in the current unit.
            for (unsigned d = 0; d < 9; ++d) {
                Mask bit = Mask(1u << d), positions = 0;
                for (unsigned p = 0; p < 9; ++p)
                    if (board[units[u][p]] & bit) positions |= Mask(1u << p);
                if (!positions) return false;
                auto remove_outside = [&](unsigned target) {
                    for (auto i : units[target]) {
                        bool inside = u < 9 ? i/9 == u : u < 18 ? i%9 == u-9 :
                            (i/27)*3+(i%9)/3 == u-18;
                        if (!inside) board[i] &= Mask(~bit);
                    }
                };
                for (unsigned g = 0; g < 3; ++g) {
                    if (!(positions & Mask(~(7u << (3*g))))) {
                        if (u < 9) remove_outside(18+(u/3)*3+g);
                        else if (u < 18) remove_outside(18+g*3+(u-9)/3);
                        else remove_outside(((u-18)/3)*3+g);
                    }
                    if (u >= 18 && !(positions & Mask(~(0x49u << g))))
                        remove_outside(9+((u-18)%3)*3+g);
                }
            }
        }
        // Revalidate newly fixed digits, even if nothing changed next time.
        if (board == before) return true;
    }
}

struct Result {
    unsigned solutions = 0;
    Board first{};
    uint64_t guesses = 0;
};
inline void search(Board board, unsigned limit, Result& result) {
    if (result.solutions >= limit || !propagate(board)) return;
    unsigned chosen = 81, best = 10;
    for (unsigned i = 0; i < 81; ++i) {
        unsigned n = std::popcount(board[i]);
        if (n > 1 && n < best) { chosen = i; best = n; }
    }
    if (chosen == 81) {
        if (!result.solutions) result.first = board;
        ++result.solutions;
        return;
    }
    ++result.guesses;
    unsigned remaining = board[chosen];
    while (remaining && result.solutions < limit) {
        Mask bit = Mask(remaining & -remaining);
        remaining &= remaining-1;
        Board child = board;
        child[chosen] = bit;
        search(child, limit, result);
    }
}
inline Result solve(const Board& board, unsigned limit = 1) {
    if (limit != 1 && limit != 2) throw std::invalid_argument("Solution limit must be 1 or 2");
    Result result;
    search(board, limit, result);
    if (result.solutions && !valid_solution(board, result.first))
        throw std::runtime_error("Invalid reference solution");
    return result;
}
} // namespace reference
