#!/usr/bin/env python3
"""Check C finite example, raw outputs, bad input and optional corpus benchmark."""
import argparse
import pathlib
import struct
import subprocess
import tempfile

from check_stream import fill, verify
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
        output = pathlib.Path(tmp) / "output.raw"
        n = RECORDS * 3 + 19
        data = bytearray(n * 64)
        fill(data, 0, n)
        source.write_bytes(data)
        subprocess.run([exe, str(source), str(output), "4", "8"], check=True)
        seen = bytearray(n)
        result = output.read_bytes()
        assert len(result) == n * 64
        verify(result, n, seen)
        assert all(seen)
        for data in (b"", bytes(63), bytes(65)):
            source.write_bytes(data)
            assert subprocess.run([exe, str(source), "-", "1", "1"]).returncode != 0
        print("PASS: C example all finite outputs/IDs/status/reserved fields; bad inputs rejected")
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
                    subprocess.run([exe, str(source), "-", str(tx), str(rx), args.seconds],
                                   check=True)


if __name__ == "__main__":
    main()
