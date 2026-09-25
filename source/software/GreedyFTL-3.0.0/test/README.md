# GreedyFTL-3.0.0 host unit tests

Compiles the FTL's pure-logic modules natively with gcc and exercises them with
[Unity](https://github.com/ThrowTheSwitch/Unity) (fetched by CMake, pinned to
`v2.6.1`). No Xilinx SDK or board is required.

## Quick start

```sh
sudo apt-get install -y cmake gcc lcov          # Debian/Ubuntu
cd source/software/GreedyFTL-3.0.0/test
cmake -S . -B build
cmake --build build --parallel
ctest --test-dir build --output-on-failure      # run all suites
cmake --build build --target coverage           # ctest + lcov HTML + summary
```

The coverage target writes `build/coverage/index.html` and prints a per-file
lcov table. Pass `-DGREEDYFTL_COVERAGE=OFF` to build without gcov
instrumentation.

## Layout

| Path | Purpose |
| --- | --- |
| `CMakeLists.txt` | Builds firmware objects with `-DHOST_TEST`, fakes, and one executable per suite. |
| `stubs/` | Header-only replacements for the Xilinx BSP (`xil_*.h`, `xparameters.h`, `xscugic.h`, ...). |
| `fakes/fake_regs.c` | In-memory register map behind `Xil_In32/Xil_Out32` and `IO_READ32/IO_WRITE32`; records every write so tests can assert on it. |
| `fakes/fake_nsc_driver.c` | Replaces `nsc_driver.c`: in-memory NAND model (per-row read/program, per-block erase; storage allocated per block on first program), bad-block list, and program/erase/ECC fault injection. |
| `fakes/fake_host_lld.c` | Replaces `host_lld.c`: logs DMA descriptors and NVMe completions, models the command FIFO. |
| `fakes/host_memory.c` | Below-4 GiB arena that backs the `memory_map.h` addresses under `HOST_TEST`. |
| `support/ftl_test_env.c` | Fixture: resets all fakes, runs `InitFTL()`, drains the scheduler, issues production-path writes. |
| `tests/` | One Unity suite per module. |

## How the firmware is built for the host

`HOST_TEST` is the only switch. It affects four production headers
(`memory_map.h`, `nvme/io_access.h`, `nvme/debug.h`, `nvme/nvme.h`) and does
nothing when the firmware is built for the Zynq. Everything else is compiled
unmodified. `xparameters.h` in `stubs/` exposes two NAND channels x 8 ways
(16 dies), which keeps the FTL tables small but non-trivial.

Firmware stores DRAM pointers in `unsigned int`, so the host arena must live
below 4 GiB; `host_memory.c` uses `MAP_32BIT`/a fixed hint to guarantee that.
This makes the harness **Linux-only**: macOS reserves the whole low 4 GiB
(`__PAGEZERO`), so every test aborts in `host_memory_init()` there. On a Mac,
run the suite in a Linux container (e.g. `docker run -v $PWD:/src ubuntu:24.04`).

## Writing a test

```c
#include "unity.h"
#include "ftl_test_env.h"

void setUp(void)    { ftl_test_env_init(); }   /* fresh FTL every test */
void tearDown(void) {}

static void test_example(void)
{
	unsigned int vsa = ftl_test_write_slice(/*lsa*/ 7, /*fill*/ 0xA5);
	ftl_test_drain();
	TEST_ASSERT_EQUAL_UINT32(vsa, AddrTransRead(7));
}
```

Useful hooks: `fake_nand_fail_next_program()`, `fake_nand_mark_bad()`,
`fake_dma_count()/fake_dma_at()`, `fake_nvme_cpl_at()`,
`fake_reg_writes_to()`. Tests that document a known defect use
`TEST_IGNORE_MESSAGE` so the suite stays green while the bug stays visible.

## Troubleshooting

* `scheduler did not converge` from `ftl_test_drain()`: a request is blocked
  (usually a row-address dependency). Create data through
  `ftl_test_write_slice()` instead of poking FTL tables directly.
* `libgcov profiling error: ... stamp mismatch`: stale `.gcda` after a
  rebuild; `cmake --build build --target coverage` zeroes counters first, or
  `find build -name '*.gcda' -delete`.
* `mmap` failure in `host_memory_init()`: the process could not get a
  below-4 GiB mapping; make sure ASLR/`ulimit -v` are not restricting it, or
  you are on macOS (see above).
* `lcov: ERROR: ... is unused` / `RC option ... is deprecated`: lcov 2.x is
  stricter than 1.x; the `coverage` target only uses patterns that match on
  both, but branch data is only reported by lcov 1.x.
