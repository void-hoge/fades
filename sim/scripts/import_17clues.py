#!/usr/bin/env python3
"""Import the exact 17puz49158.txt bytes after checking puzzle/order identity."""
import argparse
import hashlib
import os
import pathlib
import tempfile

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("source", type=pathlib.Path)
args = parser.parse_args()
root = pathlib.Path(__file__).resolve().parents[2]
destination = root / "sim/data/17clues.txt"
incoming = args.source.read_bytes()
digest = hashlib.sha256(incoming).hexdigest()
expected = "58ef7d83e8cbac32495161f9745877fef82f5e8b3fe58e3cad4eb3fc004a81b9"
assert digest == expected, "Unexpected source file SHA256"
records = incoming.decode("ascii").splitlines()
assert len(records) == 49158
assert all(len(p) == 81 and set(p) <= set(".123456789") for p in records)
assert all(sum(ch != "." for ch in p) == 17 for p in records)
old = destination.read_bytes()
previous = ["".join(line.split()).replace("-", ".").replace("0", ".")
            for line in old.decode("ascii").splitlines() if line.strip()]
assert previous == records, "Puzzle contents or order differs from the existing dataset"
print("PASS: all 49,158 puzzles and their order are identical; 17 clues per puzzle")
print("Previous SHA256:", hashlib.sha256(old).hexdigest())
print("Incoming SHA256:", digest)
with tempfile.NamedTemporaryFile(dir=destination.parent, delete=False) as temp:
    name = pathlib.Path(temp.name)
    temp.write(incoming)
try:
    name.chmod(destination.stat().st_mode & 0o777)
    os.replace(name, destination)
finally:
    name.unlink(missing_ok=True)
assert destination.read_bytes() == incoming
print("Imported byte-for-byte (including CRLF):", destination)
