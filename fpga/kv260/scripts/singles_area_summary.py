#!/usr/bin/env python3
"""Extract board totals and core/engine hierarchy from the paired area builds."""
import argparse
import csv
import hashlib
import json
from pathlib import Path
import re

ROOT = Path(__file__).resolve().parents[3]
METRICS = ("luts", "logic_luts", "lutram", "srl", "ffs", "ramb36", "ramb18", "uram", "dsp")


def hierarchy(path):
    core = None
    for line in path.read_text().splitlines():
        parts = [p.strip() for p in line.split("|")[1:-1]]
        if len(parts) != 11 or not all(p.isdigit() for p in parts[2:]):
            continue
        name = parts[0]
        if re.fullmatch(r"solvers\[\d+\]\.solver", name):
            core = name
            yield "core", core, dict(zip(METRICS, map(int, parts[2:])))
        elif name in ("infer", "branching") and core:
            yield name, f"{core}/{name}", dict(zip(METRICS, map(int, parts[2:])))


def total(path):
    labels = {"CLB LUTs": "luts", "CLB LUTs*": "luts", "LUT as Logic": "logic_luts",
              "LUT as Distributed RAM": "lutram", "LUT as Shift Register": "srl",
              "CLB Registers": "ffs", "RAMB36/FIFO*": "ramb36", "RAMB18": "ramb18",
              "URAM": "uram", "DSPs": "dsp", "CLB": "clbs"}
    values = {}
    for line in path.read_text().splitlines():
        parts = [p.strip() for p in line.split("|")[1:-1]]
        if len(parts) >= 2 and parts[0] in labels:
            values.setdefault(labels[parts[0]], int(parts[1]))
    if not set(METRICS).issubset(values):
        raise ValueError(f"Incomplete utilization report: {path}")
    return values


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, default=ROOT / "results/singles_area/area.csv")
    args = parser.parse_args()
    status = json.loads((ROOT / "results/singles_area/status.json").read_text())
    if [r["cores"] for r in status["runs"] if r["state"] == "completed"] != [1, 2]:
        raise ValueError("Both full-board builds must complete before comparison")
    for name, digest in status["source_sha256"].items():
        if hashlib.sha256((ROOT / name).read_bytes()).hexdigest() != digest:
            raise ValueError(f"Source changed since measurement: {name}")
    rows = []
    for stage, prefix in (("synthesized", "synth_"), ("routed", "")):
        boards = {}
        for cores in (1, 2):
            directory = ROOT / f"build/singles-{cores}core-pl/fpga/kv260/implementation"
            boards[cores] = total(directory / f"{prefix}utilization.rpt")
            rows.append(dict(stage=stage, cores=cores, scope="board", instance="system_wrapper", **boards[cores]))
            footprints = {}
            if stage == "routed":
                with (directory / "clb_footprint.csv").open() as f:
                    for row in csv.DictReader(f):
                        instance = row["instance"].split("/stream/")[-1]
                        footprints[(row["scope"], instance)] = int(row["occupied_clbs"])
                if footprints[("board", "system_wrapper")] != boards[cores]["clbs"]:
                    raise ValueError("CLB footprint does not match board utilization")
            for scope, instance, values in hierarchy(directory / f"{prefix}hierarchy.rpt"):
                if (scope, instance) in footprints:
                    values["clbs"] = footprints[(scope, instance)]
                rows.append(dict(stage=stage, cores=cores, scope=scope, instance=instance, **values))
        rows.append(dict(stage=stage, cores="2-1", scope="board_delta", instance="incremental core plus integration",
                         **{key: boards[2][key]-boards[1][key] for key in boards[1]}))
    keys = ("stage", "cores", "scope", "instance", *METRICS, "clbs")
    args.output.parent.mkdir(parents=True, exist_ok=True)
    with args.output.open("w") as f:
        writer = csv.DictWriter(f, fieldnames=keys)
        writer.writeheader()
        writer.writerows(rows)
    print(args.output)


if __name__ == "__main__":
    main()
