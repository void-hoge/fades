#!/usr/bin/env python3
"""Build a public worktree snapshot without ignored files or generated artifacts."""
import pathlib
import re
import shutil
import subprocess
import tempfile
from urllib.parse import unquote, urlsplit

root = pathlib.Path(__file__).resolve().parents[2]
names = subprocess.check_output(
    ["git", "ls-files", "--cached", "--others", "--exclude-standard", "-z"],
    cwd=root,
).decode().split("\0")
files = sorted({name for name in names if name and (root / name).is_file()})
assert "host/tools/fades_benchmark.cpp" in files and "host/tools/fades_verify.cpp" in files
assert "docs/protocol.md" in files
assert not any(name.startswith(("build/", "results/", "tmp/")) for name in files)
assert "AGENTS.md" not in files and "note.md" not in files

out = root / "build"
out.mkdir(exist_ok=True)
log = out / "public_tree_check.log"
with tempfile.TemporaryDirectory(prefix="public-", dir=out) as temp:
    public = pathlib.Path(temp)
    for name in files:
        destination = public / name
        destination.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(root / name, destination)
    for name in files:
        if not name.endswith(".md"):
            continue
        document = public / name
        for target in re.findall(r"\]\(([^)]+)\)", document.read_text()):
            url = urlsplit(target)
            if url.scheme or not url.path:
                continue
            destination = document.parent / unquote(url.path)
            assert destination.exists(), f"Broken public link in {name}: {target}"
    with log.open("w") as output:
        print(f"Snapshot: {len(files)} files; Markdown links PASS", file=output, flush=True)
        subprocess.run(["make", "lint", "test", "synth", "host", "host-input-test"], cwd=public,
                       stdout=output, stderr=subprocess.STDOUT, check=True)
print(f"PASS public snapshot: {len(files)} files, links, lint/test/synth/host/input checks; log: {log}")
