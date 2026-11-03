# Problem files

Both files are copied directly from BinarySudoku's `problems/9x9/`, retaining
their original filenames, bytes and CRLF line endings. One puzzle per line:
81 characters in row-major order, `.` for blanks and `1`–`9` for clues.
No conversion or import script is required to use them.

|File|Puzzles|Purpose|
|---|---:|---|
|`17puz49158.txt`|49,158|17-clue corpus; default for RTL tests and performance measurements|
|`HardestDatabase110626.txt`|375|Hard 1106 corpus; difficult search-heavy puzzles|

Source commit: `a494907924083cbfc9e14a74c75f614ead752c28`.

- [Original 17puz49158.txt](https://github.com/void-hoge/binarysudoku/blob/a494907924083cbfc9e14a74c75f614ead752c28/problems/9x9/17puz49158.txt)
- [Original HardestDatabase110626.txt](https://github.com/void-hoge/binarysudoku/blob/a494907924083cbfc9e14a74c75f614ead752c28/problems/9x9/HardestDatabase110626.txt)

SHA-256:

```text
58ef7d83e8cbac32495161f9745877fef82f5e8b3fe58e3cad4eb3fc004a81b9  17puz49158.txt
ae3bce96647b90d9c3b6eec14080f9513173f562d4b6df65b1a70a8de11a0afe  HardestDatabase110626.txt
```

`.gitattributes` preserves these exact upstream bytes. `make reference-full`
checks all 49,158 puzzles in the default corpus for existence and uniqueness.
The host tools accept either file directly; BinarySudoku source code is not
needed to read or test the data.
