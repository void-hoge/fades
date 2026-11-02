# 17-clue dataset

`17clues.txt` contains 49,158 puzzles, one per line. Each line has exactly
81 characters in row-major order: `1`–`9` for clues and `.` for blanks,
without cell separators. Each puzzle has 17 clues.

The file is copied byte-for-byte from the existing workspace's
`binarysudoku/problems/9x9/17puz49158.txt`, including its CRLF line endings:
<https://github.com/void-hoge/binarysudoku/blob/a494907924083cbfc9e14a74c75f614ead752c28/problems/9x9/17puz49158.txt>.
BinarySudoku source code is not needed to read or test this dataset.

SHA-256:
`58ef7d83e8cbac32495161f9745877fef82f5e8b3fe58e3cad4eb3fc004a81b9`.

The previous whitespace/`-` representation encoded the same puzzles in the
same order. Import verified all 49,158 records before replacing it.
Both host tools and the CPU/RTL tests share a strict 81-character parser;
LF and CRLF are accepted. `.gitattributes` preserves the upstream file bytes.

To repeat the verified import from the original source file:

```sh
python3 sim/scripts/import_17clues.py /path/to/binarysudoku/problems/9x9/17puz49158.txt
```

Run `make reference-full` to check all puzzles for existence and uniqueness
with the independent CPU reference solver.
