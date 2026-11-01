#!/usr/bin/env python3
"""Summarize per-problem RTL cycle counts (CSV produced by core_benchmark)."""
import csv
import json
import math
import statistics
import sys
from collections import defaultdict


def main():
    groups = defaultdict(list)
    with open(sys.argv[1], newline="") as stream:
        for row in csv.DictReader(stream):
            groups[row["scenario"]].append(row)
    summary = {}
    for name, rows in groups.items():
        result = {"problems": len(rows)}
        for field in ("cycles", "inferences", "branches", "contradictions", "discarded", "wasted_inferences"):
            values = sorted(int(row[field]) for row in rows)
            result[field] = {
                "total": sum(values),
                "mean": statistics.mean(values),
                "median": statistics.median(values),
                "p95": values[math.ceil(len(values) * 0.95) - 1],
                "max": max(values),
            }
        result["problems_with_branches"] = sum(int(row["branches"]) > 0 for row in rows)
        result["worst_cycle_problem"] = max(rows, key=lambda row: int(row["cycles"]))["problem"]
        summary[name] = result
    # Per-problem cycles end on event_finish; the last output transfer/drain
    # cycle is reported separately in the simulator's whole-run cycle count.
    with open(sys.argv[2], "w") as stream:
        json.dump(summary, stream, indent=2)
        stream.write("\n")
    print(json.dumps(summary, indent=2))


if __name__ == "__main__":
    main()
