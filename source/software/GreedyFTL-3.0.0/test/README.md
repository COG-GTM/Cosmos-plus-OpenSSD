# GreedyFTL-3.0.0 host unit tests

Host-side (Linux, gcc) unit tests for the GreedyFTL-3.0.0 firmware. The firmware's
pure-logic modules are compiled unmodified (apart from two `#ifdef HOST_TEST` blocks,
see below) against Xilinx BSP stubs and in-memory fakes of the NAND controller, the
NVMe host DMA engine and the register file, so the FTL can be exercised and measured
with gcov/lcov without a Zynq board.

## Requirements

| Tool   | Version tested | Notes                                   |
|--------|----------------|-----------------------------------------|
| gcc    | 11.4           | any C11 gcc/clang with `--coverage`     |
| cmake  | >= 3.16        |                                         |
| lcov   | 1.15+          | provides `lcov` and `genhtml`           |
| git    | any            | CMake `FetchContent` clones Unity       |

Ubuntu: `sudo apt-get install cmake gcc lcov`

The test framework is [Unity](https://github.com/ThrowTheSwitch/Unity), pinned to tag
`v2.6.0` and fetched at configure time (`build/_deps/`). No other dependencies.

## Build, run, coverage

```sh
cd source/software/GreedyFTL-3.0.0/test
cmake -S . -B build            # FTL_COVERAGE=ON by default
cmake --build build --parallel
ctest --test-dir build --output-on-failure
cmake --build build --target coverage   # runs ctest, then lcov + genhtml
```

The `coverage` target zeroes the counters, runs all tests, and writes
`build/coverage/coverage.info`, an HTML report at `build/coverage/html/index.html`,
and prints the per-file `lcov --list` table plus the `lcov --summary` line.
Pass `-DFTL_COVERAGE=OFF` for an uninstrumented, faster build.

Individual suites are plain executables: `./build/test_address_translation`, etc.

CI runs the same steps on `ubuntu-latest` (`.github/workflows/firmware-tests.yml`),
appends the coverage table to the job summary and uploads the HTML report as the
`greedyftl-coverage-html` artifact.

## Layout

```
test/
  CMakeLists.txt          firmware object library + host support + test executables
  stubs/                  Xilinx BSP replacements (xil_*.h, xparameters*.h, xscugic*.h,
                          xtime_l.h) and the fake register file / host DRAM image
    fake_regs.[ch]        in-memory register map behind Xil_In32/Xil_Out32 and
                          IO_READ32/IO_WRITE32; records every write for assertions
    host_memory.[ch]      mmap()s a sub-4GiB "DRAM" so 32-bit firmware addresses work
    xil_stubs.c           xil_printf, XTime, XScuGic, inbyte stand-ins
  fakes/
    fake_nand.[ch]        sparse in-memory NAND: rows, block erase, factory/grown bad
                          blocks, injectable read (ECC) failures
    fake_nsc_driver.c     V2F* NAND-controller API (nsc_driver.c replacement)
    fake_host_dma.[ch]    completes NVMe DMA descriptors written to the host LLD regs
  tests/
    test_support.[ch]     fixture reset helpers (InitFTL against fresh fake NAND, ...)
    test_address_translation.c
    test_garbage_collection.c
    test_data_buffer.c
    test_request_allocation.c
    test_nvme.c
```

## What is different from the real firmware

* **Geometry.** The board build derives `USER_CHANNELS` from the Xilinx
  `XPAR_TIGER4NSC_n_BASEADDR` symbols (8 channels). `stubs/xparameters.h` exposes
  `HOST_TEST_CHANNELS` (CMake cache variable, default 2) of them so the FTL tables
  fit comfortably in the host image and `InitFTL()` runs in well under a second.
  Everything else (`USER_WAYS=8`, blocks/pages per LUN, slice size, SLC mode) is the
  firmware's own configuration, so address math is exercised with production constants.
* **Memory map.** `memory_map.h` hard-codes DRAM addresses (`0x1000_0000`...). Under
  `HOST_TEST` those become offsets from `hostDramBase`, a region `mmap`ed below 4 GiB
  by `host_memory.c` so the firmware's `unsigned int` address arithmetic and
  pointer casts remain valid.
* **Registers.** All MMIO goes to `fake_regs.c`. Tests can assert on writes with
  `FakeRegsWriteCountTo()` / `FakeRegsLastWriteTo()`; DMA/NAND "engines" observe the
  same writes via a hook and complete requests synchronously, so no polling loop
  in `request_schedule.c` or `host_lld.c` can hang.
* **NAND.** Programmed rows are stored sparsely; erased rows read back as `0xFF`.
  Programming an already-programmed row without an erase aborts the test (real NAND
  cannot do this), so erase-before-program mistakes are caught.
  `FakeNandMarkBadBlock()` makes program/erase of a block fail (grown bad block path);
  `FakeNandInjectReadFailure()` returns an uncorrectable ECC status for one read.
* **No interrupts / timers.** `XScuGic` and `XTime` are inert stubs.

## Firmware source edits (host-only, `#ifdef HOST_TEST`)

1. `memory_map.h` — fixed DRAM base addresses replaced by `hostDramBase + offset`.
2. `nvme/io_access.h` — `IO_READ32/IO_WRITE32` route to `FakeRegRead32/FakeRegWrite32`.

The non-`HOST_TEST` branches are byte-for-byte the original code.

## Known firmware hazards documented by tests

* **`PutToFreeReqQ()` accepts an already-free slot** (`request_allocation.c`).
  Releasing the same request twice links the slot to itself in the free list and
  over-counts `freeReqQ.reqCnt`; a subsequent `GetFromFreeReqQ()` can hand the same
  tag out twice. `test_request_allocation.c` has
  `test_double_free_is_rejected` marked `TEST_IGNORE` (the intended behaviour)
  plus `test_double_free_currently_corrupts_free_queue_characterization` that pins the
  current behaviour so any fix is visible. The firmware was intentionally left
  unchanged.

## Troubleshooting

* `lcov: no valid records found` — the tests did not run, or the build was
  configured with `-DFTL_COVERAGE=OFF`. Re-run `cmake --build build --target coverage`.
* `libgcov profiling error: overwriting an existing profile data with a different
  timestamp` when running a test binary by hand — harmless; the `coverage` target
  zeroes counters first.
* `host_memory: could not map ... below 4 GiB` — the process could not reserve a fixed
  low address range (unusual sandboxes). Check `ulimit -v` / `vm.mmap_min_addr`.
* Unity fetch fails offline — pre-populate `build/_deps/unity-src` from a checkout
  of Unity `v2.6.0`, or set `FETCHCONTENT_SOURCE_DIR_UNITY`.
