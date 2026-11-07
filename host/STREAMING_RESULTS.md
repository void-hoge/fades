# Streaming validation

## ABI v3: fixed slots and zero-copy output — 2026-11-08 JST

Public slots are fixed at **16,384 records (1 MiB), eight per direction**.
Input submission accepts all records or none. Output combines independent input
groups and ends at capacity or the last registered result while idle. A short
slot does not seal input. TX credits include work still waiting in the host/DMA;
the final-beat marker is frozen before backpressure can expose it.

`SolverStream::try_receive()` returns a move-only RX loan. Applications read
the cached mapping directly; release/destruction requeues the slot. A lifetime
token keeps mappings/fd valid if a loan outlives its stream, while DMA is stopped.
The class contains no benchmark deadline, input source or validation callback.

### Hardware

KV260 revB, six cores at 115,384,615 Hz, DMA requested 187.5 MHz, 128-bit AXI,
64-byte records; Ubuntu 24.04.5, kernel 6.8.0-1015-xilinx. Native tools use
GCC 13.3.0 Release with Cortex-A53 scheduling. Vivado 2026.1 standard CMake build
and firmware checks passed: **WNS +0.037 ns / WHS +0.011 ns**, TNS/THS zero,
reviewed CDC/skew/DRC and fan TTC0 wave[2] → EMIO → A12 preserved.
Final utilization: **90,856 LUT (77.58%), 37,380 FF (15.96%), 14,052 CLB
(95.98%), 128 BRAM tiles (88.89%)**. No additional BRAM was needed.

### Final measurement scope and result

```sh
build/kv260-ps/host/fades_benchmark sim/data/17puz49158.txt 10
```

The final check completed **187,554,154 requests in 10.000939 seconds**:
**18.753654 Mproblem/s**, with a **0.000785-second** final drain and Errors=0.
Every timed result's entire **64 bytes** is read into an observable additive
checksum (3006349485991893908), before returning its RX slot. Timing includes
input copying/unique ID updates, DMA, solving, all-result reads, RX recycling,
kernel cache synchronization and final drain. There is **no output copy**.

File parsing/encoding, CPU reference preparation, device setup and independent
pre/post correctness sessions are excluded. Pre/post sessions each checked
**131,072 unique IDs**, status, Sudoku constraints, hints and reserved bits.
Timed results are consumed into the checksum rather than fully Sudoku-validated;
DMA lengths and final cumulative stage/per-core counts are also checked.
The file is cycled; counts include repeated requests. This is one final
10-second check, not a three-run median. No new dmesg messages occurred.

Historical 27.62 Mproblem/s measurements below did not read all timed results on
the CPU and therefore have a different scope.

### Read-path investigation (intermediate implementations)

|Read path|Mproblem/s|Scope|
|---|---:|---|
|Noncached RX + ordinary copy|2.585965|All output copied to application buffers|
|Noncached RX + four-vector load|1.092022|All output copied; rejected|
|Cached RX + ordinary copy|14.907604|All output copied|
|Cached RX + NEON/prefetch copy|16.806529|All output copied; median of 3 runs|
|RX loans + all-byte additive checksum|18.753654|Final zero-copy API, all output read|

RX uses `dma_alloc_pages` / `dma_mmap_pages`, with bidirectional DMA ownership
synchronization before transfer, after actual completion, and after cancellation.
This permits cacheable CPU reads without userspace cache maintenance or falsely
declaring the noncoherent HP ports coherent. TX and SG descriptors retain their
coherent allocation. The copied-output intermediate also passed a 60-second
run of 1,011,851,882 requests, but that is not the final zero-copy measurement.

### Validation

- RTL full-set/Hard stream tests, AXI-Lite and lint passed, including merged
  output, short idle tails, resumed input and stable stalled TLAST after new
  credits. Directed tests crossed 2^32 for credit accumulation and output comparison.
- Host CTests passed on x86 and Arm: constructor rollback, slot all-or-none,
  zero-copy pointer identity, move/replacement/exactly-once return, loans that
  outlive their stream, and deferred automatic-recycling errors.
- Raw ABI and real-board tests: **9,000+9,000 → 16,384+1,616**, idle resume
  without END_INPUT, 422,835 mixed IDs, dirty cached RX reuse, SG wrap,
  RX starvation, cancellation and restart.
- Public class test: **280,147 unique IDs**, borrowed RX slots, oversized/busy
  submissions, automatic return, idle tails and resumed input/backpressure.
- ioctl file example: **400,005 records**, final short slot, empty/truncated file.
- Final zero-copy verifier: warmup 49,158 + continuous **147,474** full-set
  results; Hard 375 mixed (311 solved, 32 unsatisfiable, 32 invalid), warmup
  and measured sessions. Every answer/ID was checked against the CPU reference.
- Native kernel module build and Kbuild W=1 passed.

Local logs: `results/streaming16/fixed-slots-loan-final.log` (final),
`fixed-slots-final.log` (copied-output checks), and the read-path trial logs.

## Historical ABI v2 streaming validation — 2026-11-07

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

## C++ API review follow-up

After converting Device to constructor-initialized RAII, documenting the API,
renaming the session/result helpers and replacing assertion-style error checks,
the host and KV260 builds and both host CTests passed. The new helper test injects
open/INFO/ABI/TX-mmap/RX-mmap failures, verifies rollback and destructor cleanup,
and distinguishes empty completion queues from ioctl errors. It also checks the
copy helper's payload preservation and IDs across a 32-bit carry on x86 and Arm.
The ioctl example's 400,005 records and the continuous full-set/mixed verifier
checks passed again on KV260, with no new dmesg output.

A first 10-second spot check was 27.522271 Mproblem/s. The subsequent three
10-second runs were **27.582902 / 27.619979 / 27.620254 Mproblem/s**, median
**27.619979 Mproblem/s**. All had Errors=0 and passed the 80,000-record warmup
and final resident-buffer checks. This retains the approximately 27.62
Mproblem/s throughput of the original implementation. Raw follow-up logs are
`results/streaming16/review-validation.log` and `review-benchmark.log`.
