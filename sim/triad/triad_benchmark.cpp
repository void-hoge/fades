// Algorithm/microarchitecture exploration only. No Vivado or board access.
// Snapshot-parallel policies consume the SAME input state. Forwarding policies
// explicitly compare optimistic single-tick fusion with a two-tick update.
// Frequency and area remain unproven.
#include "sudoku.hpp"
#include "triad.hpp"
#include <algorithm>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

namespace {
struct Policy {
    const char* name;
    bool parallel, early_solution, commit;
    unsigned direct_rules = 0; // 1=naked, 2=+hidden, 3=+locked; same snapshot
    bool seed_clues = false;
    bool lazy_triad = false;
    bool cell_branch = false;
    bool direct_only = false;
    bool forward_messages = false;
    bool recompute_positive = false;
    unsigned update_cost = 1;
};
struct Stats {
    std::uint64_t infer = 0, branches = 0, contradictions = 0, skipped = 0;
    std::uint64_t initialization = 0;
    std::uint64_t forced_conflict_wait = 0;
    unsigned depth = 0;
};
// Direct cell rules bypass the long C -> N -> P -> positive triad -> C path.
// Every analysis below reads s, and all eliminations are intersected at the
// end. This is one snapshot update, not sequential reference::propagate().
triad::Step direct_step(const triad::State& s, unsigned rules, bool* forced_conflict = nullptr) {
    triad::Step result{s, triad::cell_bad(s)};
    if (forced_conflict) *forced_conflict = false;
    std::array<triad::Mask, 27> used{}, unique{};
    std::array<triad::Mask, 81> removed{};
    for (unsigned u = 0; u < 27; ++u) {
        triad::Mask seen = 0, repeated = 0;
        for (auto c : reference::units[u]) {
            auto m = s.cells[c];
            if (std::has_single_bit(m)) used[u] |= m;
            repeated |= seen & m;
            seen |= m;
        }
        unique[u] = seen & triad::Mask(~repeated);
    }
    if (rules >= 3) {
        for (unsigned u = 0; u < 27; ++u) {
            for (unsigned d = 0; d < 9; ++d) {
                unsigned positions = 0;
                for (unsigned p = 0; p < 9; ++p)
                    if (s.cells[reference::units[u][p]] & (1u << d)) positions |= 1u << p;
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
    }
    for (unsigned c = 0; c < 81; ++c) {
        unsigned r = c/9, col = c%9, box = r/3*3+col/3;
        auto own = std::has_single_bit(s.cells[c]) ? s.cells[c] : 0;
        auto peer = (used[r] | used[9+col] | used[18+box]) & ~own;
        auto forced = rules >= 2 ? s.cells[c] & (unique[r] | unique[9+col] | unique[18+box]) : 0;
        if (forced && !std::has_single_bit(unsigned(forced))) {
            result.contradiction = true;
            if (forced_conflict) *forced_conflict = true;
        }
        result.state.cells[c] &= triad::Mask(~(peer | removed[c]));
        if (forced) result.state.cells[c] &= forced;
    }
    return result;
}
triad::State seeded(const reference::Board& input) {
    auto s = triad::initial(input);
    for (unsigned c = 0; c < 81; ++c) {
        auto m = input[c];
        if (!std::has_single_bit(m)) continue;
        unsigned r = c/9, col = c%9, box = r/3*3+col/3;
        for (unsigned p = 0; p < 9; ++p)
            if (triad::cell(box,p) != c) s.cells[triad::cell(box,p)] &= triad::Mask(~m);
        s.negative[6*box+r%3] &= triad::Mask(~m);
        s.negative[6*box+3+col%3] &= triad::Mask(~m);
        for (unsigned o = 0; o < 2; ++o) {
            unsigned band = o == 0 ? r/3 : col/3;
            unsigned peer = o == 0 ? col/3 : r/3;
            unsigned elem = o == 0 ? r%3 : col%3;
            for (unsigned f = 0; f < 6; ++f)
                if (triad::permutations[f][peer] != elem)
                    s.configs[6*(3*o+band)+f] &= triad::Mask(~m);
        }
    }
    return s;
}
bool complete(const triad::State& s) {
    for (auto c : s.cells) if (!std::has_single_bit(c)) return false;
    return true;
}
// A completed cell assignment is usable before auxiliary closure only if it
// also satisfies every stored N/P restriction (including rejected configs).
bool compatible_solution(const triad::State& s) {
    if (triad::cell_bad(s)) return false;
    for (unsigned box = 0; box < 9; ++box) {
        for (unsigned t = 0; t < 6; ++t) {
            triad::Mask present = 0;
            for (unsigned k = 0; k < 3; ++k)
                present |= s.cells[triad::cell(box, triad::local_cell(t/3, t%3, k))];
            if ((s.negative[6*box+t] | present) != triad::all) return false;
        }
    }
    for (unsigned o = 0; o < 2; ++o) {
        for (unsigned b = 0; b < 3; ++b) {
            for (unsigned f = 0; f < 6; ++f) {
                triad::Mask required = triad::all;
                for (unsigned peer = 0; peer < 3; ++peer) {
                    unsigned box = o == 0 ? 3*b+peer : 3*peer+b;
                    triad::Mask present = 0;
                    for (unsigned k = 0; k < 3; ++k)
                        present |= s.cells[triad::cell(box,
                            triad::local_cell(o, triad::permutations[f][peer], k))];
                    required &= present;
                }
                if ((s.configs[6*(3*o+b)+f] & required) != required) return false;
            }
        }
    }
    return true;
}
triad::Branch branch(const triad::State& s, const Policy& policy) {
    if (policy.cell_branch) {
        unsigned target = 81;
        for (unsigned c = 0; c < 81; ++c) {
            unsigned n = std::popcount(s.cells[c]);
            if (n < 2) continue;
            if (target == 81) target = c;
            if (n == 2) { target = c; break; }
        }
        triad::Branch out{s, s, target != 81};
        if (!out.valid) return out;
        auto remaining = s.cells[target];
        triad::Mask half = 0;
        for (unsigned n = std::popcount(remaining)/2; n; --n) {
            auto bit = triad::Mask(remaining & -remaining);
            half |= bit;
            remaining ^= bit;
        }
        out.a.cells[target] = half;
        out.b.cells[target] = remaining;
        return out;
    }
    auto children = triad::branch(s);
    if (!policy.commit || !children.valid) return children;
    // Reuse the selected band/digit, changing only the partition to 1/rest.
    bool first = true;
    for (unsigned i = 0; i < 36; ++i) {
        auto selected = triad::Mask(s.configs[i] ^ (children.a.configs[i] & children.b.configs[i]));
        children.a.configs[i] = s.configs[i];
        children.b.configs[i] = s.configs[i];
        if (!selected) continue;
        if (first) { children.b.configs[i] &= triad::Mask(~selected); first = false; }
        else children.a.configs[i] &= triad::Mask(~selected);
    }
    return children;
}
// Tdoku's AssertionsToEliminations forwards assertions straight to the bands.
// The initial RTL instead stores N, then discovers the same assertion again
// on a later band phase. Reuse the box assertions, rather than adding global
// Singles/Locked circuits. This model has a feed-forward logic dependency;
// 1-tick policies below are explicitly optimistic bounds, not 100 MHz claims.
triad::Step forwarded(const triad::State& s, bool recompute_positive) {
    auto box_step = triad::step(s, false);
    auto messages = s;
    for (unsigned box = 0; box < 9; ++box) {
        std::array<triad::Mask,9> ca{};
        std::array<triad::Mask,6> na{};
        for (unsigned p = 0; p < 9; ++p)
            ca[p] = triad::asserted(s.cells[triad::cell(box,p)], 1);
        for (unsigned t = 0; t < 6; ++t) {
            auto n = s.negative[6*box+t];
            na[t] = triad::asserted(n, 6);
            std::array<triad::Mask,3> c;
            for (unsigned k = 0; k < 3; ++k)
                c[k] = s.cells[triad::cell(box, triad::local_cell(t/3,t%3,k))];
            na[t] |= n & triad::Mask(~(c[0] | c[1] | c[2]));
            for (unsigned k = 0; k < 3; ++k)
                ca[triad::local_cell(t/3,t%3,k)] |= c[k] &
                    triad::Mask(~(n | c[(k+1)%3] | c[(k+2)%3]));
        }
        for (unsigned o = 0; o < 2; ++o) {
            unsigned band = o == 0 ? box/3 : box%3;
            unsigned peer = o == 0 ? box%3 : box/3;
            for (unsigned e = 0; e < 3; ++e) {
                triad::Mask positive = triad::Mask(~s.negative[6*box+3*o+e]);
                for (unsigned k = 0; k < 3; ++k)
                    positive |= ca[triad::local_cell(o,e,k)];
                auto negative = na[3*o+e];
                for (unsigned f = 0; f < 6; ++f)
                    messages.configs[6*(3*o+band)+f] &= triad::Mask(~(
                        triad::permutations[f][peer] == e ? negative : positive));
            }
        }
    }
    auto band_step = triad::step(messages, true);
    auto out = band_step;
    out.contradiction |= box_step.contradiction;
    if (recompute_positive) {
        // BandEliminate recomputes the positive view after reducing configs.
        // Only use the resulting restrictions, not another cardinality pass.
        auto view = messages;
        view.configs = band_step.state.configs;
        auto restricted = triad::step(view, true);
        out.state.cells = restricted.state.cells;
        out.state.negative = restricted.state.negative;
    }
    for (unsigned i = 0; i < 81; ++i) out.state.cells[i] &= box_step.state.cells[i];
    for (unsigned i = 0; i < 54; ++i) out.state.negative[i] &= box_step.state.negative[i];
    return out;
}
triad::Step update(const triad::State& s, const Policy& policy, bool phase,
                   bool* delayed_conflict = nullptr) {
    if (policy.forward_messages) {
        if (delayed_conflict) *delayed_conflict = false;
        return forwarded(s, policy.recompute_positive);
    }
    triad::Step direct{s, false};
    bool forced_conflict = false;
    if (policy.direct_rules) direct = direct_step(s, policy.direct_rules, &forced_conflict);
    const bool fast_only = policy.direct_only ||
        (policy.lazy_triad && (direct.contradiction || direct.state.cells != s.cells));
    auto next = fast_only ? direct : triad::step(s, phase);
    if (!fast_only && policy.parallel) {
        auto other = triad::step(s, !phase);
        next.contradiction |= other.contradiction;
        for (unsigned i = 0; i < 81; ++i) next.state.cells[i] &= other.state.cells[i];
        for (unsigned i = 0; i < 54; ++i) next.state.negative[i] &= other.state.negative[i];
        for (unsigned i = 0; i < 36; ++i) next.state.configs[i] &= other.state.configs[i];
    }
    // Match the existing cell engine's registered forced-conflict metadata.
    // Do not assume the newly added unique/force detection can halt for free
    // in the same clock. Other already-visible contradictions need no wait.
    if (delayed_conflict) *delayed_conflict = forced_conflict &&
        !(fast_only ? triad::cell_bad(s) : next.contradiction);
    if (policy.direct_rules) {
        next.contradiction |= direct.contradiction;
        for (unsigned i = 0; i < 81; ++i) next.state.cells[i] &= direct.state.cells[i];
    }
    return next;
}
void probes() {
    const Policy triad_only{"triad-only", true, false, false};
    const Policy direct{"direct-locked", true, false, false, 3};
    const Policy forward{.name="forward", .parallel=true, .early_solution=false,
                         .commit=false, .forward_messages=true};
    const Policy recompute{.name="forward-recompute", .parallel=true, .early_solution=false,
                           .commit=false, .forward_messages=true, .recompute_positive=true};
    std::cout << "rule,triad_snapshot_updates,direct_snapshot_updates,forward_updates,forward_recompute_updates\n";
    for (unsigned test = 0; test < 3; ++test) {
        reference::Board cells;
        cells.fill(511);
        if (test == 0) cells[0] = 1;
        if (test == 1) {
            cells[0] = 3;
            for (unsigned c = 1; c < 9; ++c) cells[c] = 510;
        }
        if (test == 2)
            for (unsigned r = 1; r < 3; ++r)
                for (unsigned c = 0; c < 3; ++c) cells[9*r+c] = 510;
        const char* names[] = {"naked-single-row-peer", "hidden-single-row", "locked-pointing"};
        std::cout << names[test];
        for (auto policy : {triad_only, direct, forward, recompute}) {
            auto s = triad::initial(cells);
            unsigned cycles = 0;
            auto done = [&] { return test == 1 ? s.cells[0] == 1 : !(s.cells[3] & 1); };
            while (!done() && cycles < 32) {
                auto next = update(s, policy, false);
                if (next.contradiction) throw std::runtime_error("legal probe contradicted");
                s = next.state;
                ++cycles;
            }
            if (!done()) throw std::runtime_error("probe did not propagate");
            std::cout << ',' << cycles;
        }
        std::cout << '\n';
    }
}
bool search(triad::State& s, const Policy& policy, Stats& stats, unsigned pending) {
    stats.depth = std::max(stats.depth, pending);
    bool phase = false, round_changed = false, last_changed = true;
    for (;;) {
        ++stats.infer;
        if (policy.early_solution && complete(s)) {
            if (!compatible_solution(s)) { ++stats.contradictions; return false; }
            stats.skipped += pending != 0;
            return true;
        }
        bool delayed_conflict = false;
        auto next = update(s, policy, phase, &delayed_conflict);
        stats.infer += policy.update_cost-1;
        if (next.contradiction) {
            if (delayed_conflict) { ++stats.infer; ++stats.forced_conflict_wait; }
            ++stats.contradictions;
            return false;
        }
        if (!last_changed) break;
        bool changed = s != next.state;
        s = next.state;
        if (policy.parallel) last_changed = changed;
        else {
            if (!phase) round_changed = changed;
            else last_changed = round_changed || changed;
            phase = !phase;
        }
    }
    if (complete(s)) { stats.skipped += pending != 0; return true; }
    auto children = branch(s, policy);
    if (!children.valid) throw std::runtime_error("stalled without branch");
    ++stats.branches;
    if (search(children.a, policy, stats, pending+1)) { s = children.a; return true; }
    if (search(children.b, policy, stats, pending)) { s = children.b; return true; }
    return false;
}
} // namespace

int main(int argc, char** argv) {
    try {
        if (argc != 2 && argc != 3) throw std::runtime_error("usage: triad_benchmark DATASET [POLICY] | --probes");
        if (std::string(argv[1]) == "--probes") { probes(); return 0; }
        std::ifstream input(argv[1]);
        if (!input) throw std::runtime_error("cannot open dataset");
        std::vector<reference::Board> puzzles, answers;
        std::string line;
        while (std::getline(input, line)) {
            auto p = reference::parse(line);
            auto answer = reference::solve(p);
            if (!answer.solutions) throw std::runtime_error("dataset contains unsatisfiable puzzle");
            puzzles.push_back(p); answers.push_back(answer.first);
        }
        const Policy policies[] = {
            {"alternating-balanced", false, false, false},
            {"parallel-balanced", true, false, false},
            {"parallel-commit", true, false, true},
            {"parallel-early-balanced", true, true, false},
            {"parallel-early-commit", true, true, true},
            {"parallel-naked", true, false, false, 1},
            {"parallel-singles", true, false, false, 2},
            {"parallel-singles-locked", true, false, false, 3},
            {"parallel-singles-seeded", true, false, false, 2, true},
            {"parallel-singles-locked-seeded", true, false, false, 3, true},
            {"parallel-singles-early", true, true, false, 2},
            {"parallel-singles-locked-early", true, true, false, 3},
            {"parallel-singles-locked-seeded-early", true, true, false, 3, true},
            {"lazy-locked-early", true, true, false, 3, false, true},
            {"parallel-locked-early-cell", true, true, false, 3, false, false, true},
            {"lazy-locked-early-cell", true, true, false, 3, false, true, true},
            {"legacy-cell-model", true, true, false, 3, false, false, true, true},
            {.name="forward-1tick-bound", .parallel=true, .early_solution=true,
             .commit=false, .forward_messages=true},
            {.name="forward-recompute-1tick-bound", .parallel=true, .early_solution=true,
             .commit=false, .forward_messages=true, .recompute_positive=true},
            {.name="forward-2tick", .parallel=true, .early_solution=true,
             .commit=false, .forward_messages=true, .update_cost=2},
            {.name="forward-recompute-2tick", .parallel=true, .early_solution=true,
             .commit=false, .forward_messages=true, .recompute_positive=true, .update_cost=2},
        };
        std::cout << "policy,puzzles,infer,branches,contradictions,skip_events,max_depth,initialization,forced_conflict_wait,"
                      "estimated_core_cycles,cycles_per_puzzle,estimated_6core_100MHz_Mproblem_s\n";
        bool matched = false;
        for (auto policy : policies) {
            if (argc == 3 && std::string(argv[2]) != policy.name) continue;
            matched = true;
            Stats stats;
            for (unsigned p = 0; p < puzzles.size(); ++p) {
                auto s = policy.seed_clues ? seeded(puzzles[p]) : triad::initial(puzzles[p]);
                stats.initialization += policy.seed_clues;
                if (!search(s, policy, stats, 0) || s.cells != answers[p])
                    throw std::runtime_error(std::string(policy.name)+": independent solution mismatch");
            }
            // Calibrated against the alternating RTL on the 17-clue corpus:
            // inference + branch commits + 3 cycles after sibling discard +
            // initial root load. Assumes reads prefetched before branch pops;
            // this is approximate (a few boundary cycles differ) and omits
            // additional RAM stalls that faster inference could introduce.
            auto cycles = stats.initialization + stats.infer + stats.branches + 3*stats.skipped + 1;
            std::cout << policy.name << ',' << puzzles.size() << ',' << stats.infer << ','
                      << stats.branches << ',' << stats.contradictions << ',' << stats.skipped
                      << ',' << stats.depth << ',' << stats.initialization << ',' << stats.forced_conflict_wait
                      << ',' << cycles << ',' << double(cycles)/puzzles.size()
                      << ',' << 600.0*puzzles.size()/cycles << '\n';
        }
        if (!matched) throw std::runtime_error("unknown benchmark policy");
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
