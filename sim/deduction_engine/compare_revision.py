#!/usr/bin/env python3
"""Prove one-update equivalence against a committed engine in modes 0/1/2.

Uses Yosys elaboration and SAT only; does not run technology synthesis,
placement, or routing. All 729 candidate bits are unconstrained, including
malformed and contradictory states. Logs and the frozen reference are saved.
"""
import argparse
from pathlib import Path
import re
import shutil
import subprocess


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("revision", help="Git revision containing the reference engine")
    parser.add_argument("output", type=Path, help="directory for reference, miters and SAT logs")
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[2]
    yosys = shutil.which("yosys")
    if yosys is None:
        parser.error("yosys is required")
    revision = subprocess.check_output(
        ["git", "rev-parse", "--verify", f"{args.revision}^{{commit}}"], cwd=root, text=True
    ).strip()
    reference = subprocess.check_output(
        ["git", "show", f"{revision}:rtl/deduction_engine.sv"], cwd=root, text=True
    )
    modules = re.findall(r"^module\s+(\w+)", reference, re.MULTILINE)
    names = re.compile(r"\b(" + "|".join(map(re.escape, modules)) + r")\b")
    reference = names.sub(lambda match: "reference_" + match[0], reference)
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    frozen = output / "reference.sv"
    frozen.write_text(reference)
    (output / "reference_revision.txt").write_text(revision + "\n")
    for mode in range(3):
        script = output / f"mode-{mode}.ys"
        script.write_text(
            f'read_verilog -sv "{frozen}" "{root / "rtl/deduction_engine.sv"}"\n'
            f"chparam -set SEGMENT_RULES {mode} reference_deduction_engine\n"
            f"chparam -set SEGMENT_RULES {mode} deduction_engine\n"
            "miter -equiv -flatten reference_deduction_engine deduction_engine equivalence\n"
            "prep -top equivalence\n"
            "sat -verify -prove trigger 0 -show-inputs\n"
        )
        log = output / f"mode-{mode}.log"
        subprocess.run([yosys, "-Q", "-q", "-l", str(log), "-s", str(script)], check=True)
        print(f"PASS mode {mode}: all engine outputs equivalent to {revision}; {log}", flush=True)


if __name__ == "__main__":
    main()
