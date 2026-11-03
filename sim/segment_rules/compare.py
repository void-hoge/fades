#!/usr/bin/env python3
"""Compare complete core-test dataset traces, excluding stress scenarios."""
import argparse
import csv
import json
import math
from pathlib import Path


def load(path):
    with path.open(newline="") as stream:
        rows = [row for row in csv.DictReader(stream) if row["scenario"] == "dataset"]
    totals = json.loads(Path(str(path) + ".totals.json").read_text())["dataset"]
    cycles = {row["problem"]: int(row["cycles"]) for row in rows}
    if len(rows) != totals["problems"] or len(cycles) != len(rows) or not rows:
        raise ValueError(f"Incomplete or duplicate dataset trace: {path}")
    return cycles, totals


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("baseline", type=Path)
    parser.add_argument("variants", type=Path, nargs="+")
    args = parser.parse_args()
    baseline, base_totals = load(args.baseline)
    report = {}
    for path in [args.baseline, *args.variants]:
        cycles, totals = load(path)
        if cycles.keys() != baseline.keys():
            raise ValueError(f"Dataset mismatch: {path}")
        ordered = sorted(cycles.values())
        worst = max(cycles, key=lambda name: cycles[name] / baseline[name])
        report[str(path)] = {
            "problems": len(cycles),
            "cycles": totals["cycles"],
            "cycles_per_problem": totals["cycles"] / len(cycles),
            "inferences": totals["inferences"],
            "branches": totals["branches"],
            "same_clock_speedup": base_totals["cycles"] / totals["cycles"],
            "min_clock_ratio_same_core_count": totals["cycles"] / base_totals["cycles"],
            "throughput_ratio_five_vs_baseline_six": 5 / 6 * base_totals["cycles"] / totals["cycles"],
            "faster_problems": sum(cycles[name] < baseline[name] for name in cycles),
            "slower_problems": sum(cycles[name] > baseline[name] for name in cycles),
            "unchanged_problems": sum(cycles[name] == baseline[name] for name in cycles),
            "p95_completion_interval": ordered[math.ceil(len(ordered) * 0.95) - 1],
            "max_completion_interval": ordered[-1],
            "worst_relative_problem": worst,
            "worst_relative_cycle_ratio": cycles[worst] / baseline[worst],
        }
    print(json.dumps(report, indent=2))


if __name__ == "__main__":
    main()
