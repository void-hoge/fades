#!/usr/bin/env python3
"""Run paired RTL benchmarks and retain per-problem data plus summary CSV."""
import argparse
from collections import Counter
import concurrent.futures
import csv
from datetime import datetime, timezone
import hashlib
import json
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[2]


def run_one(binary_dir, output, label, dataset, scope, mode):
    stem = f"{label}-{scope}-{mode}"
    path = output / f"{stem}.csv"
    command = [str(binary_dir / f"{scope}_bench_{mode}"), str(dataset), str(path)]
    result = subprocess.run(command, capture_output=True, text=True)
    (output / f"{stem}.log").write_text(result.stdout + result.stderr)
    if result.returncode:
        raise RuntimeError(f"{stem}: {result.stdout}{result.stderr}")
    histogram = Counter()
    keys = ("inferences", "branches", "contradictions", "memory_wait", "solved", "stalled")
    totals = dict.fromkeys(keys, 0)
    with path.open() as f:
        for row in csv.DictReader(f):
            histogram[int(row["cycles"])] += 1
            for key in keys:
                totals[key] += int(row[key])
    n = sum(histogram.values())
    total = sum(c * count for c, count in histogram.items())
    def percentile(p):
        rank = (p*n+99)//100
        count = 0
        for value, frequency in sorted(histogram.items()):
            count += frequency
            if count >= rank:
                return value
    summary = dict(dataset=label, scope=scope, mode=mode, problems=n,
                   cycles=total, mean_cycles=total/n,
                   p50=percentile(50), p95=percentile(95),
                   p99=percentile(99), maximum=max(histogram))
    summary.update(totals)
    print(result.stdout.strip(), stem, flush=True)
    return summary


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build", type=Path, default=ROOT / "build/host")
    parser.add_argument("--output", type=Path, default=ROOT / "results/singles_cycles")
    parser.add_argument("--jobs", type=int, default=2)
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    datasets = {"kaggle1million": ROOT / "sim/data/kaggle1million.csv",
                "17clue": ROOT / "sim/data/17puz49158.txt",
                "hard1106": ROOT / "sim/data/HardestDatabase110626.txt"}
    metadata = {label: dict(path=str(path), sha256=hashlib.file_digest(path.open("rb"), "sha256").hexdigest())
                for label, path in datasets.items()}
    metadata["git_revision"] = subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=ROOT, text=True).strip()
    metadata["verilator"] = subprocess.check_output(["verilator", "--version"], text=True).strip()
    metadata["measured_at_utc"] = datetime.now(timezone.utc).isoformat()
    metadata["source_sha256"] = {str(path.relative_to(ROOT)):
                                 hashlib.file_digest(path.open("rb"), "sha256").hexdigest()
                                 for path in (ROOT / "rtl/deduction_engine.sv", ROOT / "rtl/fades_core.sv",
                                              ROOT / "rtl/branch_generator.sv", Path(__file__).with_name("cycle_benchmark.cpp"))}
    (args.output / "metadata.json").write_text(json.dumps(metadata, indent=2) + "\n")
    with concurrent.futures.ThreadPoolExecutor(max_workers=args.jobs) as pool:
        futures = [pool.submit(run_one, args.build / "sim/deduction_engine", args.output,
                               label, path, scope, mode)
                   for label, path in datasets.items()
                   for scope in ("deduction", "core") for mode in ("singles", "locked")]
        summaries = [f.result() for f in futures]
    with (args.output / "summary.csv").open("w") as f:
        writer = csv.DictWriter(f, fieldnames=summaries[0].keys())
        writer.writeheader()
        writer.writerows(summaries)


if __name__ == "__main__":
    main()
