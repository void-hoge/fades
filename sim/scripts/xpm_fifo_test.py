#!/usr/bin/env python3
"""Exercise the production XPM FIFO, not the portable Verilator FIFO model."""
import pathlib
import shutil
import subprocess

root = pathlib.Path(__file__).resolve().parents[2]
compiler = shutil.which("xvlog")
if compiler is None:
    raise SystemExit("Vivado xvlog must be on PATH")
vivado = pathlib.Path(compiler).resolve().parents[1]
out = root / "build/fifo_xpm"
out.mkdir(parents=True, exist_ok=True)
xpm = vivado / "data/ip/xpm"
commands = [
    [compiler, "--sv", *(str(xpm / name / "hdl" / f"{name}.sv")
                         for name in ("xpm_cdc", "xpm_memory", "xpm_fifo"))],
    [compiler, "--sv", "--define", "SYNTHESIS",
     str(root / "rtl/async_fifo.sv"), str(root / "sim/tb/tb_async_fifo_xpm.sv")],
    [compiler, str(vivado / "data/verilog/src/glbl.v")],
    ["xelab", "tb_async_fifo_xpm", "glbl", "-L", "unisims_ver", "-s", "async_fifo_sim"],
    ["xsim", "async_fifo_sim", "-runall"],
]
log = out / "test.log"
with log.open("w") as output:
    for command in commands:
        subprocess.run(command, cwd=out, stdout=output, stderr=subprocess.STDOUT, check=True)
text = log.read_text()
assert "PASS actual XPM:" in text and "Fatal:" not in text, f"XPM test failed: {log}"
print(f"PASS actual XPM async FIFO; log: {log}")
