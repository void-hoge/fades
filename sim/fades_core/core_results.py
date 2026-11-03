#!/usr/bin/env python3
"""Summarize core-test CSVs or compare a full run with a saved baseline."""
import argparse
import csv
import json
import math
import statistics
from collections import defaultdict
from pathlib import Path


def summarize(path, output):
    groups = defaultdict(list)
    with path.open(newline="") as stream:
        for row in csv.DictReader(stream):
            groups[row["scenario"]].append(row)
    summary = {}
    for name, rows in groups.items():
        result = {"problems": len(rows)}
        for field in ("cycles", "inferences", "branches", "contradictions", "discarded", "wasted_inferences"):
            values = sorted(int(row[field]) for row in rows)
            result[field] = {
                "total": sum(values), "mean": statistics.mean(values),
                "median": statistics.median(values),
                "p95": values[math.ceil(len(values)*0.95)-1], "max": max(values),
            }
        result["problems_with_branches"] = sum(int(row["branches"]) > 0 for row in rows)
        result["worst_cycle_problem"] = max(rows, key=lambda row: int(row["cycles"]))["problem"]
        summary[name] = result
    # CSV intervals end on event_finish. Final output drain is in .totals.json.
    text = json.dumps(summary, indent=2)+"\n"
    output.write_text(text)
    print(text, end="")


def compare(baseline, current):
    for path in (baseline, current):
        with path.open(newline="") as stream:
            rows = list(csv.DictReader(stream))
        counts = {name: sum(row["scenario"] == name for row in rows)
                  for name in ("dataset", "stress")}
        if counts != {"dataset": 49158, "stress": 4096} or len(rows) != 53254:
            raise ValueError(f"Incomplete full run: {path}: {counts}")
    if baseline.read_bytes() != current.read_bytes():
        raise AssertionError("Per-problem traces differ")
    totals = [json.loads(Path(str(path)+".totals.json").read_text()) for path in (baseline, current)]
    if totals[0] != totals[1]:
        raise AssertionError("Run totals differ")
    print("PASS: all 49158 puzzles + 4096 stress cases; identical CSV and run totals")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="command", required=True)
    summary = commands.add_parser("summary", help="write cycle/event statistics")
    summary.add_argument("csv", type=Path)
    summary.add_argument("output", type=Path)
    regression = commands.add_parser("compare", help="check full CSV and whole-run totals")
    regression.add_argument("baseline", type=Path)
    regression.add_argument("current", type=Path)
    args = parser.parse_args()
    if args.command == "summary":
        summarize(args.csv, args.output)
    else:
        compare(args.baseline, args.current)
