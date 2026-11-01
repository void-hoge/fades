# 17-clue dataset

`17clues.txt` contains 49,158 puzzles, one per line. Each line has 81
whitespace-separated tokens in row-major order: `1`–`9` for clues and `-`
for blanks. Each puzzle has 17 clues.

The file is copied byte-for-byte from the existing workspace's
`binarysudoku/problems/9x9/17clues.txt`:
<https://github.com/void-hoge/binarysudoku/blob/a494907924083cbfc9e14a74c75f614ead752c28/problems/9x9/17clues.txt>.
BinarySudoku source code is not needed to read or test this dataset.

SHA-256:
`63069ab8989d5039eb57f268dca875278b728f28fbc3ea8642ab678898245174`.

Run `make reference-full` to check all puzzles for existence and uniqueness
with the independent CPU reference solver.
