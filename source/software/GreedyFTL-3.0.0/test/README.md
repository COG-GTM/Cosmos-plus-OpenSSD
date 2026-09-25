# GreedyFTL-3.0.0 host unit tests

Builds the pure-logic parts of the GreedyFTL firmware natively on a Linux host
(gcc + CMake) and runs them against fake hardware, so FTL behaviour can be
tested and measured without a Zynq board or the Xilinx SDK.

## Quick start

```bash
# from the repository root
cmake -S source/software/GreedyFTL-3.0.0/test -B build/greedyftl-test
cmake --build build/greedyftl-test -j
ctest --test-dir build/greedyftl-test --output-on-failure
cmake --build build/greedyftl-test --target coverage   # lcov + HTML report
```

Requirements: gcc, cmake >= 3.16, `lcov`/`genhtml` (for the `coverage` target)
and network access on first configure (Unity is fetched via `FetchContent`,
pinned to tag `v2.6.0`).

Coverage output lands in `build/greedyftl-test/coverage/`:
`summary.txt` (lcov `--list` + `--summary`), `greedyftl.info` (tracefile, firmware
sources only) and `html/index.html`.

## Layout

| Path | Purpose |
| --- | --- |
| `CMakeLists.txt` | Builds firmware sources with `-DHOST_TEST=1` and gcov flags into `libgreedyftl_fw.a`; one CTest per `tests/test_*.c`. |
| `stubs/` | Header replacements for the Xilinx BSP (`xil_io.h`, `xil_printf.h`, `xparameters.h`, `xtime_l.h`, `xil_cache.h`, `xscugic.h`, ...). `Xil_In32/Xil_Out32` and `IO_READ32/IO_WRITE32` go through the fake register map. `host_memory_map.h` replaces the fixed DRAM layout. |
| `fakes/host_mem.c` | `mmap`s a <4 GiB arena so the firmware's `unsigned int` DRAM addresses are real host pointers; table layout relative to `DRAM_START_ADDR` is unchanged. |
| `fakes/fake_regs.c` | Sparse register map with a write journal, host-DMA command FIFO capture and NVMe completion counting. Tests assert on register writes and DMA descriptors. |
| `fakes/nsc_driver.c` | In-memory NAND behind the `V2F*` driver API: lazy per-page storage, erase-to-0xFF, program/read/erase, factory/grown bad-block marks, injectable failures, op counters. |
| `tests/test_support.c` | Shared fixture: boots the FTL once, snapshots the DRAM arena and restores it per test; helpers to write/read slices through the real request queues. |
| `tests/test_*.c` | Unity suites (see below). |

## Suites

| Suite | What it covers |
| --- | --- |
| `test_address_translation` | LBA -> VSA -> PSA round trip, die striping, `FindFreeVirtualSlice`, `InvalidateOldVsa`, free-block reserve, `EraseBlock`, factory/grown bad-block remap. |
| `test_garbage_collection` | Victim list bookkeeping, victim = most invalid slices, fully-invalid block freed with zero copies, only valid pages copied. |
| `test_data_buffer` | Hit/miss, hash chains, LRU order, eviction, blocking-request chains, dirty vs clean eviction write-back. |
| `test_request_allocation` | Alloc until exhaustion, release, FIFO queues, selective removal, NAND completion counters, double-free. |
| `test_request_schedule` | Way priority lists, row/data-buffer address generation, row-address dependency (write order, read-after-program, erase gating), end-to-end issue. |
| `test_nvme_cmd` | `ReqTransNvmeToSlice` splitting, I/O opcode dispatch, Identify controller/namespace + PRP DMA chunking, Set/Get Features, create/delete I/O queues. |

Tests marked `TEST_IGNORE` document existing firmware behaviour that looks
wrong; they are deliberately not "fixed" in the harness. See the PR description
or the comment above each ignored test.

## Firmware source changes for host builds

Only three headers were touched, each adding an `#ifdef HOST_TEST` branch that
includes a `stubs/` header; the production path is byte-for-byte unchanged:

- `memory_map.h` -> `host_memory_map.h` (runtime DRAM base instead of `0x00100000`)
- `nvme/io_access.h` -> `host_io_access.h` (`IO_READ32/IO_WRITE32` via fake registers)
- `nvme/nvme.h` -> `host_admin_cmd_buffer.h` (`ADMIN_CMD_DRAM_DATA_BUFFER` inside the arena)

## Troubleshooting

- **`lcov: ERROR: ... mismatch`** on lcov 2.x: the CMake target already passes
  `--ignore-errors`; make sure `gcov` matches the `gcc` used to build.
- **`mmap` fails / addresses above 4 GiB**: `host_mem.c` requires `MAP_32BIT`
  (x86-64) or a kernel that honours the low hint address. Run on x86-64 Linux.
- **Stale `.gcda` timestamp warnings** after editing sources: run the
  `coverage` target (it zeroes counters) or delete the build directory.
- **Unity fetch fails offline**: pre-populate `build/greedyftl-test/_deps` or
  point `FETCHCONTENT_SOURCE_DIR_UNITY` at a local Unity `v2.6.0` checkout.
