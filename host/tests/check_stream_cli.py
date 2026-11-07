#!/usr/bin/env python3
"""Run the ioctl example on the board, including >mmap capacity and short EOF."""
import pathlib
import struct
import subprocess
import sys
import tempfile

binary = pathlib.Path(sys.argv[1]).resolve()
digits = "534678912672195348198342567859761423426853791713924856961537284287419635345286179"
cells = sum(int(d) << (4*i) for i, d in enumerate(digits))
with tempfile.TemporaryDirectory(prefix="stream-cli-", dir=binary.parent) as tmp:
    source = pathlib.Path(tmp) / "input.bin"
    target = pathlib.Path(tmp) / "output.bin"
    count = 400005
    with source.open("wb") as f:
        for rid in range(count):
            f.write(struct.pack("<Q", rid) + cells.to_bytes(41, "little") + bytes(15))
    subprocess.run([binary, source, target, "10000"], check=True, timeout=120)
    seen = bytearray(count)
    with target.open("rb") as f:
        while r := f.read(64):
            assert len(r) == 64
            rid = int.from_bytes(r[:8], "little")
            assert rid < count and not seen[rid]
            seen[rid] = 1
            assert r[8] == 0 and int.from_bytes(r[9:50], "little") == cells
            assert r[50:] == bytes(14)
    assert all(seen)
    source.write_bytes(b"")
    subprocess.run([binary, source, target], check=True, timeout=10)
    assert target.stat().st_size == 0
    source.write_bytes(bytes(65))
    p = subprocess.run([binary, source, target], capture_output=True, text=True, timeout=10)
    assert p.returncode != 0 and "truncated" in p.stderr
print("PASS: ioctl example, 400005 IDs, final short packet, empty/truncated input")
