#!/usr/bin/env python3
"""Compare complete runs with different branch policies (ordering stays fixed)."""
import argparse
import csv
import json
from pathlib import Path


def load(path):
    with path.open(newline="") as stream:
        rows = list(csv.DictReader(stream))
    totals = json.loads(Path(str(path) + ".totals.json").read_text())
    groups = {}
    for name, count in (("dataset", 49158), ("stress", 4096)):
        group = [row for row in rows if row["scenario"] == name]
        if len(group) != count or totals[name]["problems"] != count:
            raise ValueError(f"Incomplete {name} run: {path}")
        if sum(int(row["cycles"]) for row in group) + 1 != totals[name]["cycles"]:
            raise ValueError(f"Cycle accounting mismatch: {path}")
        groups[name] = group
    if len(rows) != 49158 + 4096:
        raise ValueError(f"Unexpected scenario: {path}")
    if [row["problem"] for row in groups["dataset"]] != [f"dataset_{i}" for i in range(1, 49159)]:
        raise ValueError(f"Missing or reordered dataset problems: {path}")
    return groups, totals


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("baseline", type=Path)
    parser.add_argument("current", type=Path)
    parser.add_argument("output", type=Path)
    args = parser.parse_args()
    before, old_totals = load(args.baseline)
    after, new_totals = load(args.current)
    report = {"baseline": str(args.baseline), "current": str(args.current)}
    for name in ("dataset", "stress"):
        if [row["problem"] for row in before[name]] != [row["problem"] for row in after[name]]:
            raise ValueError(f"Different problem order in {name}")
        for field in ("prefill", "deque_depth"):
            if old_totals[name][field] != new_totals[name][field]:
                raise ValueError("Different queue configuration")
        delta = [int(b["cycles"]) - int(a["cycles"]) for a, b in zip(before[name], after[name])]
        report[name] = {
            "rows_identical": before[name] == after[name],
            "totals_identical": old_totals[name] == new_totals[name],
            "baseline_totals": old_totals[name], "current_totals": new_totals[name],
            "baseline_mean_cycles": old_totals[name]["cycles"] / len(delta),
            "current_mean_cycles": new_totals[name]["cycles"] / len(delta),
            "cycle_delta": new_totals[name]["cycles"] - old_totals[name]["cycles"],
            "shorter_completion_intervals": sum(d < 0 for d in delta),
            "longer_completion_intervals": sum(d > 0 for d in delta),
            "equal_completion_intervals": sum(d == 0 for d in delta),
        }
    text = json.dumps(report, indent=2) + "\n"
    args.output.write_text(text)
    print(text, end="")


if __name__ == "__main__":
    main()
