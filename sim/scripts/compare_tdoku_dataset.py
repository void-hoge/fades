#!/usr/bin/env python3
"""Compare puzzle text with Tdoku's archive and optionally export 81-char lines."""
import argparse
import collections
import hashlib
import pathlib
import zipfile

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("puzzles", type=pathlib.Path)
parser.add_argument("tdoku_zip", type=pathlib.Path)
parser.add_argument("--output", type=pathlib.Path, help="write normalized 81-character input")
args = parser.parse_args()

local = ["".join(line.split()).replace("-", ".").replace("0", ".")
         for line in args.puzzles.read_text().splitlines() if line.strip()]
with zipfile.ZipFile(args.tdoku_zip) as archive:
    candidates = [name for name in archive.namelist()
                  if name.endswith("/puzzles6_forum_hardest_1106")
                  or name == "puzzles6_forum_hardest_1106"]
    assert len(candidates) == 1, candidates
    entry = candidates[0]
    original = archive.read(entry)
official = [line.strip()[:81].replace("0", ".")
            for line in original.decode().splitlines()
            if line.strip() and not line.lstrip().startswith("#")]
for records in (local, official):
    assert all(len(p) == 81 and set(p) <= set(".123456789") for p in records)

print("Archive entry:", entry)
print("Archive entry raw SHA256:", hashlib.sha256(original).hexdigest())
print("Local puzzles:", len(local), "Tdoku puzzles:", len(official))
print("Same puzzle multiset:", collections.Counter(local) == collections.Counter(official))
print("Same order:", local == official)
assert collections.Counter(local) == collections.Counter(official), "Different puzzle sets"
for label, records in (("Local", local), ("Tdoku", official)):
    digest = hashlib.sha256(("\n".join(records)+"\n").encode()).hexdigest()
    print(label, "normalized SHA256:", digest)
print("PASS: matching Tdoku Hard 1106 dataset")
if args.output is not None:
    args.output.write_text("\n".join(local)+"\n", encoding="ascii")
    print("Wrote canonical 81-character input:", args.output)
