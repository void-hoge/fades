# Unified record format validation

Date: 2026-11-13 (development machine).

## Format

- 64 bytes, little endian, identical input/output field positions.
- ID: bytes 0..7; status: byte 8; cells: bits 395..72 (bytes 9..49).
- Cells: row-major, 4 bits each, first cell in the low nibble.
- Input status and reserved bits are ignored; callers normally write zero.
- Output status: 0 solved, 1 unsatisfiable, 2 invalid cell value.
- Unsuccessful output cells and all output reserved bits are zero.
- Hardware identity: `0x46414404`; ioctl ABI remains v3.

## RTL

Commands:

```sh
cmake --preset host -DFADES_CORES=6
cmake --build --preset host --parallel 4
ctest --preset host -L '^(fades|fades_axi)$' --output-on-failure
```

All 7 tests passed: lint-fades, lint-fades_axi, lint-fades_kv260,
fades, fades-hard, problem-preprocessor, axi.
Integration tests include all 49,158 17-clue puzzles, the hard dataset,
invalid/unsatisfiable inputs, backpressure, reset/restart, nonzero input
status, and every reserved input bit set.

## Board build and deployment

```sh
cmake --preset kv260-pl -DFADES_CORES=6 -DFADES_VIVADO_JOBS=2
cmake --build --preset kv260-pl
```

- Actual tool selected by CMake: Vivado 2026.1.
- KV260 revB, 6 cores, kernel `6.8.0-1036-xilinx`.
- Timing constraints: core 8.666 ns, DMA 5.333 ns.
- Final WNS +0.080 ns, WHS +0.010 ns, TNS/THS 0.
- Firmware package completed, including timing/CDC/skew/fan checks.
- Assigned rates: DMA 187,500,000 Hz, core 115,384,615 Hz.
- Linux debugfs rates observed on this boot: `pl0_ref` 166,666,665 Hz,
  `pl1_ref` 111,111,110 Hz. This is a functional test, not a throughput measurement.
- Board workspace: `/root/fades/format-v4/`.
- Driver built natively against the running kernel headers.
- Loaded app `fades`, confirmed with `xmutil listapps` and `/dev/fades`.

## Board tests

```sh
python3 /root/fades/format-v4/host/tests/check_abi.py
python3 /root/fades/format-v4/host/tests/check_stream.py
```

```text
PASS: ABI, exclusive open, ioctl validation, mmap bounds/shared-only
PASS: close/reopen
PASS: 9000+9000 -> 16384+1616 and idle resume; 422835 IDs, 63 output slots, dirty cached RX reuse, SG wrap, RX starvation, cancel/restart
```

The stream test checks solved/unsatisfiable/invalid results, nonzero input
status ignored, IDs without duplicates or omissions, output cells/reserved
bytes, and session/per-core counter totals. `dmesg` showed the expected
FPGA overlay and unsigned out-of-tree module messages, with no DMA/solver
failure messages.
