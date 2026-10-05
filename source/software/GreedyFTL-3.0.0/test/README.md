# GreedyFTL host tests

## Prerequisites and commands

Install GCC, CMake, Python 3, lcov, and genhtml. Unity v2.6.0 is fetched by CMake.
On Linux:

```sh
cmake -S test -B test/build
cmake --build test/build -j
(cd test/build && ctest --output-on-failure)
cmake --build test/build --target coverage
```

On macOS with Homebrew GCC 16 and gcov 16:

```sh
cmake -S test -B test/build -DCMAKE_C_COMPILER=gcc-16 -DGCOV_TOOL=gcov-16
cmake --build test/build -j
(cd test/build && ctest --output-on-failure)
cmake --build test/build --target coverage
```

`test/build/coverage/html` contains the HTML report, `summary.txt` the lcov
summary, and `per_file.md` the line-hit table. Use
`-DGREEDYFTL_RUN_KNOWN_BUGS=ON` in a separate build directory to run opt-in bug
assertions.
Set `HOST_TEST_TIMING=1` when invoking a test executable to print each
`InitFTL()` fixture-call duration.

The fake DRAM arena is a 4-GiB-aligned address range, with the lower 1 GiB
committed for firmware tables and data. Fake NAND stores sparse programmed
pages and factory-bad marks; fake DMA completes immediately and the register
map records accesses. The fixture initializes the real FTL state and leaves
flash contents intact between tests unless a test resets fake NAND.
The host-only preincluded `host_memory_map_limits.h` adjusts the firmware's
DRAM end bound to the aligned arena so its address-range assertion compares
like address spaces.

Host-only firmware adaptations are restricted to `memory_map.h` (the three
DRAM bases and host pointer helpers), the DRAM-offset dereferences in
`address_translation.c`, `request_schedule.c`, and `nvme/nvme_identify.c`, and
the `HOST_TEST` register-access branch in `nvme/io_access.h`. Target code
continues to preprocess to its original behavior. `main.c` and
`nvme/nvme_main.c` are excluded because they own board initialization and
infinite main loops. `nsc_driver.c` is excluded because it performs
non-portable 32-bit MMIO pointer casts; its API is replaced by `fake_nand.c`.
