# GreedyFTL host tests

Host-side unit tests for the GreedyFTL-3.0.0 firmware. The real FTL sources are
compiled with `-DHOST_TEST` against fakes for DRAM, MMIO registers, NAND and
host DMA, so they run on Linux or macOS with no Cosmos+ board.

## Run

From the repository root (Linux, GCC + lcov):

```sh
cmake -S source/software/GreedyFTL-3.0.0/test -B /tmp/greedyftl-build
cmake --build /tmp/greedyftl-build -j
ctest --test-dir /tmp/greedyftl-build --output-on-failure
cmake --build /tmp/greedyftl-build --target coverage
```

On macOS, use Homebrew GCC so gcov data matches lcov:
`-DCMAKE_C_COMPILER=gcc-16 -DGCOV_TOOL=gcov-16`.

Unity v2.6.0 is downloaded as a SHA-256-pinned tarball (no git clone needed).
The `coverage` target writes `coverage/summary.txt`, `coverage/per_file.md`
and `coverage/html/` under the build directory.

## Known bugs

Tests that document real firmware bugs call `KNOWN_BUG(...)` and are reported
as IGNORED by default. Run them with
`-DGREEDYFTL_RUN_KNOWN_BUGS=ON -DENABLE_COVERAGE=OFF` in a separate build
directory; they are expected to fail (some abort or crash, by design).

## Layout

- `stubs/` - Xilinx BSP headers and host-only headers (`host_dram.h`,
  `host_reg_map.h`, `fake_nand.h`, `fake_host_dma.h`).
- `fakes/` - fake DRAM arena, register map, NAND controller (replaces
  `nsc_driver.c`), host DMA engine, BSP functions.
- `unit/` - one executable per firmware area plus `ftl_fixture.c`, which runs
  the real `InitFTL()`.

`main.c`, `nvme/nvme_main.c` (board init / infinite loops) and `nsc_driver.c`
(32-bit MMIO casts) are not compiled. Host-only firmware edits are limited to
`HOST_TEST` branches in `memory_map.h` and `nvme/io_access.h`, and
`FW_DRAM_PTR()` wrappers in `address_translation.c`, `request_schedule.c` and
`nvme/nvme_identify.c`; target builds preprocess to the original code.
