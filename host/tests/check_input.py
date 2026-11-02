#!/usr/bin/env python3
"""Check CLI names and reject malformed text before accessing the FPGA device."""
import pathlib
import subprocess
import sys
import tempfile

root = pathlib.Path(__file__).resolve().parents[2]
binary_dir = pathlib.Path(sys.argv[1]).resolve() if len(sys.argv) > 1 else root / "build/host"
tools = ((binary_dir / "fades_verify", ["1", "1", "1"]),
         (binary_dir / "fades_benchmark", ["0.001"]))
bad_inputs = [b"", b"."*80+b"\n", b"."*82+b"\n",
              b"."*81+b" \n", b" "+b"."*81+b"\n"]
bad_inputs += [b"."*40+bytes([ch])+b"."*40+b"\n" for ch in b"0- x\t\r"]

with tempfile.TemporaryDirectory(prefix="input-", dir=root / "build") as temp:
    for tool, arguments in tools:
        result = subprocess.run([str(tool)], capture_output=True, text=True)
        assert result.returncode != 0 and f"Usage: {tool.name}" in result.stderr
        for index, contents in enumerate(bad_inputs):
            path = pathlib.Path(temp) / f"bad_{index}.txt"
            path.write_bytes(contents)
            result = subprocess.run([str(tool), str(path), *arguments], capture_output=True, text=True)
            assert result.returncode != 0, (tool.name, contents)
            assert any(message in result.stderr for message in
                       ("Expected exactly 81", "Invalid cell", "Empty dataset", "Puzzle file is empty")), result.stderr
        print(f"PASS {tool.name}: Usage and {len(bad_inputs)} malformed input cases")
