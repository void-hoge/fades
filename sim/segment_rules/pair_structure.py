#!/usr/bin/env python3
"""Count an explicitly constructed Boolean DAG, without FPGA synthesis.

Scope: 81 cells' empty/single/fixed features, 54 segment E/M summaries,
162 fixed cell pairs and 54 reserved masks. Compare alternative factorizations.
Uses two-input AND nodes and complemented edges (AIG), balanced reductions,
and structural hashing only. No rewriting, LUT mapping, placement or timing.
These are expression-structure metrics, NOT estimates of actual LUT or CLB.
"""
import argparse
from collections import Counter
import json
from pathlib import Path


class Graph:
    def __init__(self):
        self.nodes = [None]  # false=0, true=1; each literal uses bit 0 for inversion
        self.shared = {}

    def input(self):
        self.nodes.append(None)
        return 2 * (len(self.nodes)-1)

    def and_(self, a, b):
        if a == 0 or b == 0 or a == (b ^ 1):
            return 0
        if a == 1 or a == b:
            return b
        if b == 1:
            return a
        key = tuple(sorted((a, b)))
        if key not in self.shared:
            self.nodes.append(key)
            self.shared[key] = 2 * (len(self.nodes)-1)
        return self.shared[key]

    def or_(self, a, b):
        return self.and_(a ^ 1, b ^ 1) ^ 1

    def xor(self, a, b):
        return self.or_(self.and_(a, b ^ 1), self.and_(a ^ 1, b))

    def any(self, bits):
        bits = list(bits)
        while len(bits) > 1:
            bits = [self.or_(bits[i], bits[i+1]) if i+1 < len(bits) else bits[i]
                    for i in range(0, len(bits), 2)]
        return bits[0] if bits else 0

    def clear_lowest(self, bits):
        # Exact arithmetic identity: output[i] = bits[i] & OR(bits[0:i]).
        # Count this direct prefix form rather than charging redundant XOR
        # gates for subtract-and-mask. Use the same form for both designs.
        borrow = 1
        result = []
        for bit in bits:
            result.append(self.and_(bit, borrow ^ 1))
            borrow = self.and_(borrow, bit ^ 1)
        return result

    def features(self, bits):
        rest = self.clear_lowest(bits)
        nonzero = self.any(bits)
        rest_nonzero = self.any(rest)
        one = self.and_(nonzero, rest_nonzero ^ 1)
        two = self.and_(rest_nonzero, self.any(self.clear_lowest(rest)) ^ 1)
        return nonzero ^ 1, one, two

    def metrics(self, outputs):
        reachable = set()
        fanout = Counter(lit // 2 for lit in outputs)

        def visit(lit):
            n = lit // 2
            if n in reachable:
                return
            reachable.add(n)
            if self.nodes[n] is not None:
                for arg in self.nodes[n]:
                    fanout[arg // 2] += 1
                    visit(arg)

        for lit in outputs:
            visit(lit)
        depth = {0: 0}
        for n in sorted(reachable):
            node = self.nodes[n]
            depth[n] = 0 if node is None else 1 + max(depth[arg // 2] for arg in node)
        gates = [n for n in reachable if self.nodes[n] is not None]
        return {"and_nodes": len(gates), "max_and_depth": max(depth.values()),
                "max_gate_fanout": max(fanout[n] for n in gates)}


def measure(variant):
    g = Graph()
    cells = [[g.input() for _ in range(9)] for _ in range(81)]
    features = [g.features(bits) for bits in cells]
    outputs = []
    for bits, (empty, one, _) in zip(cells, features):
        outputs.extend([empty, one, *[g.and_(bit, one) for bit in bits]])
    for orientation in range(2):
        for line in range(9):
            for part in range(3):
                indices = [line*9+part*3+k if orientation == 0 else (part*3+k)*9+line
                           for k in range(3)]
                a, b, c = [cells[i] for i in indices]
                seen = [g.any((x, y, z)) for x, y, z in zip(a, b, c)]
                multi = [g.any((g.and_(x, y), g.and_(x, z), g.and_(y, z)))
                         for x, y, z in zip(a, b, c)]
                outputs.extend(seen + multi)
                if variant == "cell_only":
                    continue
                masks = []
                pair_flags = []
                for excluded in range(3):
                    left, right = ((1, 2), (0, 2), (0, 1))[excluded]
                    i, j = indices[left], indices[right]
                    a, b = cells[i], cells[j]
                    if variant == "shared_cardinality":
                        a_contains_b = g.any(g.and_(y, x ^ 1) for x, y in zip(a, b)) ^ 1
                        b_contains_a = g.any(g.and_(x, y ^ 1) for x, y in zip(a, b)) ^ 1
                        separate = g.and_(g.and_(features[i][1], features[j][1]), a_contains_b ^ 1)
                        use_a = g.or_(g.and_(features[i][2], a_contains_b), separate)
                        use_b = g.or_(g.and_(features[j][2], b_contains_a), separate)
                        mask = [g.or_(g.and_(x, use_a), g.and_(y, use_b)) for x, y in zip(a, b)]
                    elif variant in ("bivalue_only", "bivalue_shared_multi"):
                        # Both domains have size two, so one containment
                        # direction is sufficient to establish equality.
                        subset = g.any(g.and_(x, y ^ 1) for x, y in zip(a, b)) ^ 1
                        take = g.and_(g.and_(features[i][2], features[j][2]), subset)
                        pair_flags.append(take)
                        mask = [g.and_(bit, take) for bit in a]
                    else:
                        union = [g.or_(x, y) for x, y in zip(a, b)]
                        two = g.features(union)[2]
                        mask = [g.and_(bit, two) for bit in union]
                    masks.append(mask)
                    outputs.extend(mask)
                if variant == "bivalue_shared_multi":
                    active = g.any(pair_flags)
                    outputs.extend(g.and_(bit, active) for bit in multi)
                else:
                    outputs.extend(g.any(mask[d] for mask in masks) for d in range(9))
    return g.metrics(outputs)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    base, old, new, bivalue, multi = [measure(name) for name in
                                     ("cell_only", "union_counting", "shared_cardinality",
                                      "bivalue_only", "bivalue_shared_multi")]
    result = {"scope": "cell empty/single/fixed + segment E/M + pairs/reserved only",
              "mapping": "unmapped structurally hashed AIG; not FPGA resources/timing",
              "cell_only": base, "union_counting": old, "shared_cardinality": new,
              "bivalue_only": bivalue,
              "bivalue_shared_multi": multi,
              "shared_and_node_reduction_fraction": 1-new["and_nodes"]/old["and_nodes"],
              "bivalue_and_node_reduction_fraction": 1-bivalue["and_nodes"]/old["and_nodes"],
              "multi_total_and_node_reduction_fraction": 1-multi["and_nodes"]/old["and_nodes"],
              "multi_incremental_and_node_reduction_fraction":
                  1-(multi["and_nodes"]-base["and_nodes"])/(old["and_nodes"]-base["and_nodes"]),
              "incremental_pair_and_nodes": {name: metrics["and_nodes"]-base["and_nodes"]
                                             for name, metrics in (("union_counting", old),
                                                                   ("shared_cardinality", new),
                                                                   ("bivalue_only", bivalue),
                                                                   ("bivalue_shared_multi", multi))}}
    text = json.dumps(result, indent=2) + "\n"
    if args.output:
        args.output.write_text(text)
    print(text, end="")


if __name__ == "__main__":
    main()
