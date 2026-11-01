#!/usr/bin/env python3
"""Compare complete RTL traces and run totals with a saved baseline."""
import argparse
import csv
import json
from pathlib import Path


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("baseline", type=Path)
    parser.add_argument("current", type=Path)
    args = parser.parse_args()
    for path in (args.baseline, args.current):
        with path.open(newline="") as stream:
            rows = list(csv.DictReader(stream))
        counts = {name: sum(row["scenario"] == name for row in rows)
                  for name in ("dataset", "stress")}
        if counts != {"dataset": 49158, "stress": 4096} or len(rows) != 53254:
            raise ValueError(f"Incomplete full run: {path}: {counts}")
    if args.baseline.read_bytes() != args.current.read_bytes():
        raise AssertionError("Per-problem traces differ")
    totals = [json.loads(Path(str(path) + ".totals.json").read_text())
              for path in (args.baseline, args.current)]
    if totals[0] != totals[1]:
        raise AssertionError("Run totals differ")
    print("PASS: all 49158 puzzles + 4096 stress cases; identical CSV and run totals")


if __name__ == "__main__":
    main()
