#include "segment_model.hpp"
#include <fstream>
#include <iomanip>
#include <iostream>
#include <string>
#include <vector>

int main(int argc, char** argv) {
    try {
        if (argc != 2 && argc != 3)
            throw std::runtime_error("usage: segment_benchmark DATASET [PER_PROBLEM.csv]");
        std::ifstream input(argv[1]);
        if (!input) throw std::runtime_error("cannot open dataset");
        std::vector<reference::Board> puzzles, answers;
        std::string line;
        while (std::getline(input,line)) {
            auto board = reference::parse(line);
            auto oracle = reference::solve(board);
            if (!oracle.solutions) throw std::runtime_error("dataset must contain solvable puzzles");
            puzzles.push_back(board); answers.push_back(oracle.first);
        }
        if (puzzles.empty()) throw std::runtime_error("empty dataset");
        std::ofstream detail;
        if (argc == 3) {
            detail.open(argv[2]);
            if (!detail) throw std::runtime_error("cannot open per-problem CSV");
            detail << "mode,problem,infer,branches,contradictions,extra_removed,segment_conflict_cycles,"
                      "pair_extra_removed,pair_at_triple_stalls,pair_bits_at_triple_stalls\n";
        }
        std::cout << "mode,problems,infer,branches,contradictions,skip_events,max_pending,"
                     "tight_segments,useful_segments,useful_cycles,extra_removed,segment_conflict_cycles,"
                     "estimated_cycles,cycles_per_problem,relative_throughput,"
                     "tight_pairs,pair_useful_cycles,pair_extra_removed,"
                     "pair_at_triple_stalls,pair_bits_at_triple_stalls\n";
        std::uint64_t baseline_cycles = 0;
        const char* names[] = {"baseline","segment-contradiction","segment-triples","segment-pairs-triples",
                               "segment-bivalue-pairs"};
        for (unsigned variant = 0; variant < 5; ++variant) {
            segment_model::Stats total;
            for (unsigned p = 0; p < puzzles.size(); ++p) {
                auto board = puzzles[p];
                auto before = total;
                if (!segment_model::search(board, segment_model::Mode(variant), total) || board != answers[p])
                    throw std::runtime_error(std::string(names[variant])+": reference mismatch at "+std::to_string(p+1));
                if (detail.is_open()) detail << names[variant] << ',' << p+1 << ',' << total.infer-before.infer
                    << ',' << total.branches-before.branches << ',' << total.contradictions-before.contradictions
                    << ',' << total.extra_removed-before.extra_removed << ','
                    << total.segment_conflict_cycles-before.segment_conflict_cycles << ','
                    << total.pair_extra_removed-before.pair_extra_removed << ','
                    << total.pair_at_triple_stalls-before.pair_at_triple_stalls << ','
                    << total.pair_bits_at_triple_stalls-before.pair_bits_at_triple_stalls << '\n';
            }
            // Match core's infer/branch accounting. Three cycles per discard
            // approximate root reload; additional branch-pop RAM stalls are
            // not modeled. This estimate is not an implemented clock rate.
            auto cycles = total.infer + total.branches + 3*total.skip_events + 1;
            if (variant == 0) baseline_cycles = cycles;
            std::cout << names[variant] << ',' << puzzles.size() << ',' << total.infer << ','
                      << total.branches << ',' << total.contradictions << ',' << total.skip_events << ','
                      << total.max_pending << ',' << total.tight_segments << ',' << total.useful_segments << ','
                      << total.useful_cycles << ',' << total.extra_removed << ',' << total.segment_conflict_cycles
                      << ',' << cycles << ',' << std::fixed << std::setprecision(6)
                      << double(cycles)/puzzles.size() << ',' << double(baseline_cycles)/cycles << ','
                      << total.tight_pairs << ',' << total.pair_useful_cycles << ','
                      << total.pair_extra_removed << ',' << total.pair_at_triple_stalls << ','
                      << total.pair_bits_at_triple_stalls << std::endl;
        }
        if (argc == 3) { detail.flush(); if (!detail) throw std::runtime_error("cannot write per-problem CSV"); }
    } catch (const std::exception& e) {
        std::cerr << "FAIL: " << e.what() << '\n';
        return 1;
    }
}
