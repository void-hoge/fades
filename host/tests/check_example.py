#!/usr/bin/env python3
"""Smoke-check the streaming tool with generated input or a puzzle corpus."""
import argparse
import pathlib
import struct
import subprocess
import tempfile

from check_stream import fill
from fades_device import RECORDS


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("executable")
    p.add_argument("--corpus", type=pathlib.Path)
    p.add_argument("--seconds", default="3")
    p.add_argument("--repeat", type=int, default=1)
    args = p.parse_args()
    exe = str(pathlib.Path(args.executable).resolve())
    with tempfile.TemporaryDirectory(prefix="fades-example-") as tmp:
        source = pathlib.Path(tmp) / "input.raw"
        n = RECORDS * 3 + 19
        data = bytearray(n * 64)
        fill(data, 0, n)
        if args.corpus:
            data = bytearray()
            for line in args.corpus.read_text().splitlines():
                line = line.strip()
                if not line:
                    continue
                assert len(line) == 81 and set(line) <= set(".0123456789")
                cells = sum((0 if c == "." else int(c)) << (4 * i)
                            for i, c in enumerate(line))
                data += struct.pack("<QB", len(data) // 64, 0)
                data += cells.to_bytes(41, "little") + bytes(14)
        source.write_bytes(data)
        for _ in range(args.repeat):
            for tx, rx in ((8, 8), (8, 64)):
                subprocess.run([exe, str(source), str(tx), str(rx), args.seconds], check=True)
        for data in (b"", bytes(63), bytes(65)):
            source.write_bytes(data)
            assert subprocess.run([exe, str(source), "1", "1", args.seconds]).returncode != 0
        print("PASS: streaming tool completed; bad inputs rejected")


if __name__ == "__main__":
    main()
