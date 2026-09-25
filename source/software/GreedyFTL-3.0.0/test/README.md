# GreedyFTL-3.0.0 host-native unit tests

Builds the firmware's pure-logic C sources with the host `gcc`, links them
against mocks of the Zynq BSP / NAND controller / NVMe host controller, and
runs [Unity](https://github.com/ThrowTheSwitch/Unity) tests with gcov/lcov
coverage. No Xilinx SDK or hardware is required.

## Quick start

```sh
sudo apt-get install -y cmake gcc lcov        # Ubuntu 22.04: cmake 3.22, lcov 1.14
cmake -S source/software/GreedyFTL-3.0.0/test -B build/greedyftl-test
cmake --build build/greedyftl-test -j
ctest --test-dir build/greedyftl-test --output-on-failure
cmake --build build/greedyftl-test --target coverage   # tests + lcov summary + HTML
```

Coverage outputs land in `build/greedyftl-test/coverage/`:
`summary.txt` (`lcov --summary`), `per_file.txt` (`lcov --list`),
`coverage.info` and `html/index.html`.

Per-module line: `grep <file>.c build/greedyftl-test/coverage/per_file.txt`.

### CMake options

| Option | Default | Meaning |
| --- | --- | --- |
| `GREEDYFTL_TEST_CHANNELS` | `2` | NAND channels exposed by the stub `xparameters.h` (1..8). CI also runs 8 (production topology). |
| `GREEDYFTL_COVERAGE` | `ON` | Instrument firmware sources with `--coverage -O0`. |

Set `GREEDYFTL_TEST_VERBOSE=1` in the environment to see firmware `xil_printf` output.

## What is compiled

| Firmware source | Built as | Notes |
| --- | --- | --- |
| `address_translation.c`, `data_buffer.c`, `ftl_config.c`, `garbage_collection.c`, `request_allocation.c`, `request_schedule.c`, `request_transform.c`, `nvme/nvme_admin_cmd.c`, `nvme/nvme_identify.c`, `nvme/nvme_io_cmd.c`, `nvme/nvme_main.c` | real code (`greedyftl_fw`) | unmodified |
| `nsc_driver.c` | replaced by `mocks/mock_nsc.c` | V2F NAND controller API |
| `nvme/host_lld.c` | replaced by `mocks/mock_host_lld.c` in FTL tests; real code in `harness/test_harness_host_lld.c` | NVMe host controller + DMA |
| `main.c` | not built | Zynq cache / MMU / interrupt bring-up only |

No firmware file is modified. Three techniques make that possible:

1. **Fixed-address DRAM window** (`mocks/fw_memory.c`). The firmware stores
   metadata at hard-coded DDR addresses from `memory_map.h` (e.g.
   `DATA_BUFFER_MAP_ADDR 0x18000000`) and keeps buffer addresses in
   `unsigned int`. The harness `mmap`s `[0x00100000, 0x40000000)` at those
   exact addresses (`MAP_FIXED_NOREPLACE | MAP_NORESERVE`), so firmware
   pointers work unchanged on a 64-bit host. `fw_memory_reset()` zeroes it
   between tests with `madvise(MADV_DONTNEED)`; only touched pages cost RAM.
2. **Force-included prelude** (`stubs/greedyftl_test_prelude.h`, passed with
   `-include`). It pre-defines the include guards of `nvme/io_access.h` and
   `nvme/debug.h` so that `IO_READ32`/`IO_WRITE32` go to `mocks/mock_io.c`
   (logged, scriptable register file) and `ASSERT`/`assert` call
   `fw_assert_fail()` instead of spinning forever.
3. **BSP stubs** (`stubs/`): `xil_types.h`, `xil_printf.h/.c`, `xil_io.h`,
   `xil_cache.h`, `xil_mmu.h`, `xil_exception.h`, `xscugic*.h`,
   `xtime_l.h/.c` (deterministic clock), `xparameters*.h`.

## Writing tests

Add `unit/test_<module>.c`; CMake globs `unit/test_*.c`, one executable per
file, so module owners never need to edit `CMakeLists.txt`.

```c
#include "unity.h"
#include "fw_test.h"      /* all mocks + fw_test_reset()/fw_test_init_ftl() */
#include "memory_map.h"

void setUp(void)    { fw_test_reset(); fw_test_init_ftl(); }
void tearDown(void) {}

static void test_write_then_read_maps_lsa(void)
{
	unsigned int vsa = AddrTransWrite(5);
	TEST_ASSERT_EQUAL_HEX32(vsa, AddrTransRead(5));
}

int main(void) { UNITY_BEGIN(); RUN_TEST(test_write_then_read_maps_lsa); return UNITY_END(); }
```

Helpers:

| Header | Use |
| --- | --- |
| `fw_test.h` | `fw_test_reset()` (zero DRAM window, reset every mock, `g_nvmeTask`, the scripted `inbyte()` and the `FindDieForFreeSliceAllocation()` round-robin cursor), `fw_test_init_ftl()` (runs real `InitFTL()` against an ideal NAND, then clears the `mock_nsc` call log/counters). |
| `fw_assert.h` | `FW_EXPECT_ASSERT(stmt)` — pass iff `stmt` trips a firmware `ASSERT`/`assert`. `FW_RUN_UNTIL_LOOP_EXIT(stmt)` + `fw_loop_exit()` — escape `while(1)` loops such as `nvme_main()` from a mock hook. |
| `fw_memory.h` | `fw_ptr(addr)` — host pointer for a firmware DRAM address. |
| `mock_io.h` | `mock_io_set_reg`, `mock_io_queue_read`, `mock_io_set_read_hook`, access log (`mock_io_log_at`, `mock_io_last_write`, counters). |
| `mock_nsc.h` | Records every V2F call (bounded log `mock_nsc_call_at`; unbounded `mock_nsc_count_cmd(V2FCommand_*)`, `mock_nsc_last_call`, `mock_nsc_call_count`; `mock_nsc_clear_calls`); script busy/ready, status report, ECC error info, completion, read fill byte; per-call hook for fault injection. |
| `mock_host_lld.h` | Records completions, queue setup and DMA calls (`mock_host_count`, `mock_host_last`); `mock_host_push_cmd` feeds `get_nvme_cmd`; `mock_host_set_cc_en`, `mock_host_set_partial_done`; hook fires after each call (use with `fw_loop_exit`). |

To replace a firmware function called from another translation unit, list
it in `unit/test_<module>.wrap` (one symbol per line) and define
`__wrap_<symbol>` in the test; `__real_<symbol>` is still available.

### Found a real bug?

Do not fix firmware silently. Add the failing test, mark it with
`TEST_IGNORE_MESSAGE("BUG: <description>")` at its top, and describe it in
the PR.

## Troubleshooting

- `fw_memory: cannot map firmware DRAM window` — something already occupies
  low virtual memory (e.g. ASan shadow, `vm.mmap_min_addr` > 1 MiB). Check
  `sysctl vm.mmap_min_addr` (default 65536 is fine) and do not combine with
  sanitizers that reserve that range.
- Tests at 8 channels take ~10 s because `InitFTL()` walks every block of
  64 dies; use the default 2 channels for iteration.
- `Subroutine ... redefined at /usr/bin/geninfo` lines from lcov 1.14 on
  Ubuntu 22.04 are harmless.
