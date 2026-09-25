# GreedyFTL-3.0.0 host-native tests

Unit tests for the FTL firmware that build and run on a Linux host with plain
gcc — no Xilinx SDK, no board. The firmware `.c` files are compiled unmodified;
the Zynq BSP and the NAND/NVMe controllers are replaced by stubs and mocks.

```sh
cd source/software/GreedyFTL-3.0.0/test
cmake -S . -B build
cmake --build build -j
ctest --test-dir build --output-on-failure
cmake --build build --target coverage      # clean counters, run all, lcov + HTML
```

Coverage output lands in `build/coverage/`: `firmware.info` (lcov tracefile,
firmware sources only), `summary.txt`, and `html/index.html`.
`scripts/coverage.sh <build> <fw-dir>` produces the same report outside CMake.
Set `FTL_TEST_VERBOSE=1` to see the firmware's `xil_printf` output.

## Layout

| Path | Purpose |
|---|---|
| `stubs/` | Header replacements for the Xilinx BSP (`xil_types.h`, `xil_io.h`, `xparameters.h`, `xtime_l.h`, `xil_printf.h`, `assert.h`, ...). `ftl_test_prelude.h` is force-included into every translation unit and redirects `IO_READ32/IO_WRITE32` and `ASSERT` to the mocks. |
| `mocks/mock_io.*` | Sparse memory-mapped register model behind `Xil_In32/Out32` and `IO_READ32/IO_WRITE32`: stored values, per-address read/write handlers, and a write log so tests can assert on exact register traffic. |
| `mocks/mock_nsc.*` | Replaces `nsc_driver.c`. Every `V2F*` NAND controller call is recorded; default behaviour is an ideal, always-ready NAND with clean ECC. Programmed pages are stored per (controller, way, row) and returned by later reads until the block is erased (`mock_nsc_page_data/spare`); never-programmed pages leave the buffer untouched. Fault injection: controller busy, ready/busy masks, status-report values, transfer completion/ECC results, read-page source override. Call and IO write logs grow without bound. |
| `support/ftl_test_env.*` | Maps the firmware's fixed DRAM window (`0x00100000`–`0x40000000`, see `memory_map.h`) into the process with `mmap`, resets all state between tests, boots the full FTL (`ftl_test_env_init_ftl()` = `InitFTL()`), captures firmware `assert()`s (`FTL_TEST_EXPECT_ASSERT`), and makes host DMA complete instantly. |
| `support/xil_stubs.c` | `xil_printf`, `inbyte` (scriptable console), `XTime_GetTime`. |
| `unit/test_<module>.c` | One Unity executable per firmware module. |
| `scripts/coverage.sh` | lcov capture/filter/summary/HTML used by the CMake targets and CI. |

## Writing tests

* Call `ftl_test_env_reset()` in `setUp()`. Use `ftl_test_env_init_ftl()` when
  the module under test needs the real tables populated (address map, request
  pool, data buffers, die state). Boot takes ~0.25 s against the mocked NAND.
* Firmware pointers (`logicalSliceMapPtr`, `reqPoolPtr`, ...) point straight at
  the emulated DRAM, so tests can inspect and pre-load tables directly.
* Use `mock_io_set_read_handler()` / `mock_io_last_write()` for register
  behaviour, `mock_nsc_set_*()` to make the NAND misbehave, and
  `FTL_TEST_EXPECT_ASSERT(stmt)` to verify a firmware `ASSERT` fires.
* The test build defines two NAND channels (`stubs/xparameters.h`), so
  `USER_CHANNELS == 2`, `USER_WAYS == 8`, `USER_DIES == 16`.

## Known firmware quirks the harness works around

* `request_schedule.h` declares `dieStatusTablePtr` but `request_schedule.c`
  defines `dieStateTablePtr`; tests declare the latter themselves.
* `nvme/host_lld.h` uses `XPAR_NVMEHOSTCONTROLLER_0_BASEADDR` without
  including `xparameters.h`; the prelude includes it.
* `InitBlockDieMap()` reads one console byte (`inbyte()`); the stub returns
  `'\n'` unless input is queued. `ftl_test_env_reset()` clears the queue, so
  use `ftl_test_env_init_ftl_with_console("X")` to boot through the
  erase-everything path (`ftl_test_queue_inbyte()` is for post-boot calls).
