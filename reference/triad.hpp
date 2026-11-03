#pragma once

#include <array>
#include <bit>
#include <cstdint>
#include <stdexcept>
#include <utility>

// Scalar executable specification. Packed layout is documented in README.md;
// this representation deliberately uses indexed masks rather than RTL bit slices.
namespace triad {
using Mask = std::uint16_t;
constexpr Mask all = 511;
constexpr std::array<std::array<unsigned, 3>, 6> permutations{{
    {{0, 1, 2}}, {{1, 2, 0}}, {{2, 0, 1}},
    {{2, 1, 0}}, {{0, 2, 1}}, {{1, 0, 2}}
}};
struct State {
    std::array<Mask, 81> cells{};
    std::array<Mask, 54> negative{};
    std::array<Mask, 36> configs{};
    bool operator==(const State&) const = default;
};
inline State initial(const std::array<Mask, 81>& cells) {
    State s;
    s.cells = cells;
    s.negative.fill(all);
    s.configs.fill(all);
    return s;
}
constexpr unsigned cell(unsigned box, unsigned local) {
    return box/3*27 + box%3*3 + local/3*9 + local%3;
}
constexpr unsigned local_cell(unsigned orientation, unsigned elem, unsigned k) {
    return orientation == 0 ? elem*3+k : k*3+elem;
}
inline Mask asserted(Mask mask, unsigned minimum) {
    return std::popcount(mask) == int(minimum) ? mask : 0;
}
inline bool cell_bad(const State& s) {
    for (auto c : s.cells) if (!c) return true;
    for (unsigned u = 0; u < 27; ++u) {
        Mask seen = 0, fixed = 0;
        for (unsigned p = 0; p < 9; ++p) {
            unsigned c = u < 9 ? u*9+p : u < 18 ? p*9+u-9 : cell(u-18, p);
            auto m = s.cells[c];
            seen |= m;
            if (std::has_single_bit(m)) {
                if (fixed & m) return true;
                fixed |= m;
            }
        }
        if (seen != all) return true;
    }
    return false;
}
struct Step { State state; bool contradiction = false; };
inline Step step(const State& s, bool band_phase) {
    Step result{s, cell_bad(s)};
    auto& out = result.state;
    if (!band_phase) {
        for (unsigned box = 0; box < 9; ++box) {
            std::array<Mask, 9> forced_cell{};
            std::array<Mask, 6> forced_absent{};
            for (unsigned p = 0; p < 9; ++p)
                forced_cell[p] = asserted(s.cells[cell(box, p)], 1);
            for (unsigned t = 0; t < 6; ++t) {
                auto n = s.negative[6*box+t];
                forced_absent[t] = asserted(n, 6);
                result.contradiction |= std::popcount(n) < 6;
                // Count support in the four literals of each exactly-one clause.
                for (unsigned d = 0; d < 9; ++d) {
                    Mask bit = Mask(1u << d);
                    unsigned supports = bool(n & bit), last = 3;
                    for (unsigned k = 0; k < 3; ++k) {
                        if (s.cells[cell(box, local_cell(t/3, t%3, k))] & bit) {
                            ++supports;
                            last = k;
                        }
                    }
                    result.contradiction |= supports == 0;
                    if (supports == 1) {
                        if (last == 3) forced_absent[t] |= bit;
                        else forced_cell[local_cell(t/3, t%3, last)] |= bit;
                    }
                }
            }
            Mask used = 0;
            for (auto m : forced_cell) {
                result.contradiction |= std::popcount(m) > 1 || bool(used & m);
                used |= m;
            }
            for (unsigned t = 0; t < 6; ++t) {
                Mask present = 0;
                for (unsigned k = 0; k < 3; ++k)
                    present |= forced_cell[local_cell(t/3, t%3, k)];
                result.contradiction |= bool(present & forced_absent[t]) ||
                    std::popcount(forced_absent[t]) > 6;
                out.negative[6*box+t] &= Mask(~present);
                if (std::popcount(forced_absent[t]) == 6)
                    out.negative[6*box+t] &= forced_absent[t];
            }
            for (unsigned p = 0; p < 9; ++p) {
                Mask remove = forced_absent[p/3] | forced_absent[3+p%3];
                for (unsigned q = 0; q < 9; ++q) if (p != q) remove |= forced_cell[q];
                out.cells[cell(box, p)] &= Mask(~remove);
                if (forced_cell[p]) out.cells[cell(box, p)] &= forced_cell[p];
            }
        }
    } else {
        for (unsigned orientation = 0; orientation < 2; ++orientation) {
            for (unsigned band = 0; band < 3; ++band) {
                unsigned b = orientation*3+band;
                std::array<Mask, 9> positive{};
                Mask any = 0;
                for (unsigned f = 0; f < 6; ++f) {
                    auto m = s.configs[6*b+f];
                    any |= m;
                    for (unsigned p = 0; p < 3; ++p)
                        positive[3*p+permutations[f][p]] |= m;
                }
                result.contradiction |= any != all;
                for (unsigned peer = 0; peer < 3; ++peer) {
                    unsigned box = orientation == 0 ? band*3+peer : peer*3+band;
                    for (unsigned e = 0; e < 3; ++e) {
                        unsigned t = 6*box+3*orientation+e;
                        Mask possible = 0, elsewhere = 0;
                        for (unsigned k = 0; k < 3; ++k) {
                            auto c = cell(box, local_cell(orientation, e, k));
                            possible |= s.cells[c];
                            out.cells[c] &= positive[3*peer+e];
                            if (k != e) elsewhere |= positive[3*peer+k];
                        }
                        out.negative[t] &= elsewhere;
                        result.contradiction |= std::popcount(positive[3*peer+e]) < 3;
                        Mask forbid = Mask(~possible) | asserted(s.negative[t], 6);
                        Mask require = Mask(~s.negative[t]) | asserted(positive[3*peer+e], 3);
                        for (unsigned f = 0; f < 6; ++f)
                            out.configs[6*b+f] &= Mask(~(permutations[f][peer] == e ? forbid : require));
                    }
                }
            }
        }
    }
    return result;
}
inline bool close(State& s) {
    for (unsigned round = 0; round < 1541; ++round) {
        auto box = step(s, false);
        if (box.contradiction) return false;
        auto band = step(box.state, true);
        if (band.contradiction) return false;
        bool stable = band.state == s;
        s = band.state;
        if (stable) return true;
    }
    throw std::runtime_error("non-monotone triad propagation");
}
struct Branch { State a, b; bool valid = false; };
inline Branch branch(const State& s) {
    Branch out{s, s};
    unsigned best = 0, best_total = 55;
    for (unsigned b = 0; b < 6; ++b) {
        unsigned total = 0;
        for (unsigned f = 0; f < 6; ++f) total += std::popcount(s.configs[6*b+f]);
        if (total >= 10 && total < best_total) {
            best = b; best_total = total; out.valid = true;
        }
    }
    if (!out.valid) return out;
    unsigned digit = 0, selected = 0, best_rank = 7;
    for (unsigned d = 0; d < 9; ++d) {
        unsigned count = 0;
        for (unsigned f = 0; f < 6; ++f) count += bool(s.configs[6*best+f] & (1u << d));
        unsigned rank = count >= 4 ? 4 : count;
        if (count >= 2 && rank < best_rank) {
            digit = d; selected = count; best_rank = rank;
        }
    }
    unsigned rank = 0;
    for (unsigned f = 0; f < 6; ++f) {
        auto i = 6*best+f;
        Mask bit = Mask(1u << digit);
        if (!(s.configs[i] & bit)) continue;
        (rank++ < selected/2 ? out.b : out.a).configs[i] &= Mask(~bit);
    }
    return out;
}
inline bool solve(State& s, unsigned depth = 0) {
    if (depth > 162) throw std::runtime_error("triad search depth exceeded");
    if (!close(s)) return false;
    auto children = branch(s);
    if (!children.valid) {
        for (auto c : s.cells) if (!std::has_single_bit(c))
            throw std::runtime_error("fixed configurations without completed cells");
        return true;
    }
    if (solve(children.a, depth+1)) { s = children.a; return true; }
    if (solve(children.b, depth+1)) { s = children.b; return true; }
    return false;
}
} // namespace triad
