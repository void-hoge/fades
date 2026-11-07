# ABI v2 streaming validation — 2026-11-07

## Configuration and measurement scope

- KV260 revB, six cores at **115,384,615 Hz**, DMA **187,499,999 Hz**.
- 128-bit AXI streams, 64-byte records; eight 1 MiB mmap slots per direction.
- Benchmark transfer size: **10,000 records**. Direct AXI DMA SG tail extension.
- Ubuntu 24.04.5 / Arm64, kernel `6.8.0-1015-xilinx`.
- This build selected **Vivado 2026.1**. Final setup WNS **+0.044 ns**, hold
  WHS **+0.010 ns**, TNS/THS zero. Standard build passed timing, CDC, skew,
  DRC and the TTC0 wave[2] → EMIO → A12 fan checks.
- Native host build: GCC 13.3.0, Release, `-mcpu=cortex-a53`.

The benchmark includes ongoing input copy, unique request ID generation,
QBUF/DQBUF/poll, DMA, solving and final drain. Initial DDR preparation,
independent CPU references, warmup/final full-content validation are excluded.
The 49,158-puzzle 17-hint file is traversed cyclically, including across slot
boundaries. Counts include repeated requests, not distinct puzzles.

The historical ABI v1 median **26.597169 Msolve/s** replayed prepared DDR without
ongoing input copies/ID updates and used Vivado 2024.2. These are different
measurement conditions; do not attribute the entire difference solely to ABI.

## Final measurements

```sh
build/kv260-ps/host/fades_benchmark sim/data/17puz49158.txt 10
build/kv260-ps/host/fades_benchmark sim/data/17puz49158.txt 180
```

|Run|Requests|Actual seconds, incl. drain|Mproblem/s|Final drain seconds|
|---|---:|---:|---:|---:|
|10 s #1|276,270,000|10.002358|27.620487|0.002337|
|10 s #2|276,270,000|10.002275|27.620716|0.002234|
|10 s #3|276,290,000|10.002339|27.622540|0.002189|
|180 s|4,971,960,000|180.002351|27.621639|0.002159|

10-second median: **27.620716 Mproblem/s**. All runs: Errors=0; warmup and final
resident buffers each validated 80,000 unique IDs, Sudoku constraints/status,
hints and reserved bits. Intermediate contents are not fully checked; DMA
lengths and final cumulative PL stage/per-core totals are checked. No new
dmesg messages occurred during the final verification/measurement sequence.

The 180-second run completed **more than 2^32 requests without a reset**,
exercising 64-bit request progression, counters and continued output past the
old 32-bit total-record limit. It completed 497,196 packets. Input FIFO empty
cycles: 1,804,221; per-core input wait: approximately 1.8 million cycles over
approximately 20.77 billion core clocks. No core output-wait cycles occurred.
The 10-second input FIFO empty counts were 138,085 / 123,924 / 62,632.

## Input-supply optimization

The first generic implementation copied corpus → staging → DMA DDR and reached
11.090176 Mproblem/s. Fusing cached-source copy and ID insertion into full-width
NEON stores reached 23.683582 Mproblem/s. Cortex-A53 scheduling and bounded
eight-record-ahead prefetch then reached approximately 27.62 Mproblem/s.
The optimized operation is shared as `fades::copy_records_with_ids` in
`include/fades_stream.hpp`; it still copies new input and changes every ID.

## Correctness and lifecycle checks

- Host build and malformed-input tests passed.
- RTL stream tests on all 49,158 17-hint puzzles and Hard 1106 (375), mixed
  packet lengths, command FIFO wrap/full, output backpressure, reset/framing
  errors; AXI-Lite tests and associated stream/AXI/board lint passed.
- `check_abi.py`: invalid ioctls/buffer arguments, slot ownership, double open,
  mapping restrictions, close/reopen.
- `check_stream.py`: **422,835 IDs in 97 variable packets**, including 1, 3, 7,
  19, 31, 10,000 and 16,384 records; solved/unsatisfiable/invalid contents,
  SG ring wrap, exhausted RX followed by resume, no RX reservation, cancel,
  empty input and restart without FPGA/module reload.
- `check_stream_cli.py`: actual single-thread ioctl example, **400,005 IDs**,
  short final packet, empty file and truncated record rejection.
- `fades_verify ... 8193 6 3`: all 49,158 warmup and **147,474** continuous
  results checked against independent CPU solutions.
- `fades_verify ... 1024 3 3 mixed`: all 3,072 warmup and 9,216 continuous
  results checked, including unsatisfiable and invalid records.
- Hard 1106 `375 1 1`: all 375 warmup and 375 measured results checked.

Local raw command output is retained in `results/streaming16/kv260.log`.
