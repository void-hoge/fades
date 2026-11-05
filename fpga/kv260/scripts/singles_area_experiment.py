#!/usr/bin/env python3
"""Sequential full-board Singles-only 1/2-core builds with persistent logs."""
import argparse
from datetime import datetime, timezone
import hashlib
import json
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[3]


def now():
    return datetime.now(timezone.utc).isoformat()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--jobs", type=int, default=2)
    parser.add_argument("--vivado", default="/tools/Xilinx/Vivado/2024.2/bin/vivado")
    args = parser.parse_args()
    results = ROOT / "results/singles_area"
    results.mkdir(parents=True, exist_ok=True)
    sources = sorted((ROOT / "rtl").glob("*.sv")) + [
        ROOT / "fpga/kv260/rtl/fades_kv260.sv",
        ROOT / "fpga/kv260/scripts/build.tcl",
        ROOT / "fpga/kv260/scripts/export.tcl"]
    metadata = dict(base_revision=subprocess.check_output(
        ["git", "rev-parse", "HEAD"], cwd=ROOT, text=True).strip(), started=now(),
        branch=subprocess.check_output(["git", "branch", "--show-current"], cwd=ROOT, text=True).strip(),
        source_sha256={str(p.relative_to(ROOT)): hashlib.sha256(p.read_bytes()).hexdigest() for p in sources},
        runs=[])
    status = results / "status.json"
    for cores in (1, 2):
        build = ROOT / f"build/singles-{cores}core-pl"
        run = dict(cores=cores, locked_candidates=0, started=now(), state="running",
                   build=str(build), commands=[])
        metadata["runs"].append(run)
        status.write_text(json.dumps(metadata, indent=2) + "\n")
        commands = [
            ["cmake", "--preset", "kv260-pl", "-B", str(build), f"-DFADES_CORES={cores}",
             "-DFADES_LOCKED_CANDIDATES=0", f"-DFADES_VIVADO_JOBS={args.jobs}",
             f"-DVIVADO_EXECUTABLE={args.vivado}"],
            ["cmake", "--build", str(build), "--target", "kv260-package"]]
        for step, command in zip(("configure", "build"), commands):
            run["commands"].append(command)
            status.write_text(json.dumps(metadata, indent=2) + "\n")
            with (results / f"{cores}core-{step}.log").open("w") as log:
                result = subprocess.run(command, cwd=ROOT, stdout=log, stderr=subprocess.STDOUT)
            if result.returncode:
                run.update(state="failed", finished=now(), exit_code=result.returncode)
                status.write_text(json.dumps(metadata, indent=2) + "\n")
                raise SystemExit(f"{cores}-core {step} failed; inspect {results}")
        run.update(state="completed", finished=now())
        status.write_text(json.dumps(metadata, indent=2) + "\n")
    metadata["finished"] = now()
    status.write_text(json.dumps(metadata, indent=2) + "\n")


if __name__ == "__main__":
    main()
