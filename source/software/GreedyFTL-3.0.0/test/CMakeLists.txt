cmake_minimum_required(VERSION 3.16)
project(greedyftl_host_tests C)

# Host-native unit test build for the GreedyFTL-3.0.0 firmware.
#
#   cmake -S . -B build
#   cmake --build build
#   ctest --test-dir build --output-on-failure
#   cmake --build build --target coverage        # lcov HTML + summary
#
# The firmware's pure-logic translation units are compiled unchanged for the
# host. Hardware is replaced by:
#   stubs/   Xilinx BSP headers (xil_printf.h, xparameters.h, xil_types.h, ...)
#   mocks/   IO_READ32/IO_WRITE32 register mock, Tiger4 NSC (nsc_driver) mock
#   support/ emulated DRAM window, assert capture, InitFTL helper
# See README.md in this directory for details.

option(FTL_TEST_COVERAGE "Build with gcov instrumentation" ON)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Debug CACHE STRING "" FORCE)
endif()
# The emulated DRAM window lives at 0x00100000-0x3FFFFFFF, so the executable
# itself must not be loaded there: force position independent executables.
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
include(CheckPIESupported)
check_pie_supported(LANGUAGES C)

include(FetchContent)
FetchContent_Declare(
  unity
  GIT_REPOSITORY https://github.com/ThrowTheSwitch/Unity.git
  GIT_TAG        v2.6.0
  GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(unity)
target_compile_definitions(unity PUBLIC
  UNITY_INCLUDE_DOUBLE
  UNITY_OUTPUT_COLOR
  UNITY_SUPPORT_64)

set(FW_DIR ${CMAKE_CURRENT_SOURCE_DIR}/..)
get_filename_component(FW_DIR ${FW_DIR} ABSOLUTE)

# Firmware translation units that are pure logic (or only touch hardware
# through IO_READ32/IO_WRITE32 or the nsc_driver API).
# Excluded: main.c (Zynq boot/MMU/GIC bring-up) and nsc_driver.c (replaced by mocks/mock_nsc.c).
set(FW_SOURCES
  ${FW_DIR}/address_translation.c
  ${FW_DIR}/data_buffer.c
  ${FW_DIR}/ftl_config.c
  ${FW_DIR}/garbage_collection.c
  ${FW_DIR}/request_allocation.c
  ${FW_DIR}/request_schedule.c
  ${FW_DIR}/request_transform.c
  ${FW_DIR}/nvme/host_lld.c
  ${FW_DIR}/nvme/nvme_admin_cmd.c
  ${FW_DIR}/nvme/nvme_identify.c
  ${FW_DIR}/nvme/nvme_io_cmd.c
  ${FW_DIR}/nvme/nvme_main.c
)

set(FTL_TEST_COMMON_FLAGS -O0 -g -fno-strict-aliasing -fno-omit-frame-pointer)
if(FTL_TEST_COVERAGE)
  list(APPEND FTL_TEST_COMMON_FLAGS --coverage -fprofile-update=atomic)
  set(FTL_TEST_COVERAGE_LINK --coverage)
endif()

# ---------------------------------------------------------------------------
# Firmware under test
# ---------------------------------------------------------------------------
add_library(ftl_fw STATIC ${FW_SOURCES})
target_include_directories(ftl_fw
  PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/stubs
    ${CMAKE_CURRENT_SOURCE_DIR}/mocks
    ${CMAKE_CURRENT_SOURCE_DIR}/support
    ${FW_DIR}
    ${FW_DIR}/nvme)
target_compile_options(ftl_fw PRIVATE
  ${FTL_TEST_COMMON_FLAGS}
  -include ${CMAKE_CURRENT_SOURCE_DIR}/stubs/ftl_test_prelude.h
  # The firmware freely converts between 32-bit addresses and pointers.
  -Wno-int-to-pointer-cast -Wno-pointer-to-int-cast
  -Wno-unused-variable -Wno-unused-but-set-variable -Wno-implicit-function-declaration)
target_compile_definitions(ftl_fw PUBLIC FTL_HOST_TEST=1)
target_link_options(ftl_fw PUBLIC ${FTL_TEST_COVERAGE_LINK})

# ---------------------------------------------------------------------------
# Stubs, mocks and harness support
# ---------------------------------------------------------------------------
add_library(ftl_test_support STATIC
  mocks/mock_io.c
  mocks/mock_nsc.c
  support/xil_stubs.c
  support/ftl_test_env.c)
target_include_directories(ftl_test_support PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/stubs
  ${CMAKE_CURRENT_SOURCE_DIR}/mocks
  ${CMAKE_CURRENT_SOURCE_DIR}/support
  ${FW_DIR}
  ${FW_DIR}/nvme)
target_compile_options(ftl_test_support PRIVATE
  ${FTL_TEST_COMMON_FLAGS} -Wall -Wextra
  -include ${CMAKE_CURRENT_SOURCE_DIR}/stubs/ftl_test_prelude.h
  -Wno-int-to-pointer-cast -Wno-pointer-to-int-cast)
# The firmware calls into the stubs (xil_printf, mocks) and the harness calls
# into the firmware, so the two static libraries are mutually dependent.
target_link_libraries(ftl_test_support PUBLIC ftl_fw unity)
target_link_libraries(ftl_fw PUBLIC ftl_test_support)

# ---------------------------------------------------------------------------
# Unit tests: one executable per firmware module (test/unit/test_<module>.c)
# ---------------------------------------------------------------------------
enable_testing()

function(add_ftl_test name)
  add_executable(${name} unit/${name}.c ${ARGN})
  target_compile_options(${name} PRIVATE
    ${FTL_TEST_COMMON_FLAGS} -Wall -Wextra
    -include ${CMAKE_CURRENT_SOURCE_DIR}/stubs/ftl_test_prelude.h
    -Wno-int-to-pointer-cast -Wno-pointer-to-int-cast)
  target_link_libraries(${name} PRIVATE ftl_test_support unity)
  set_target_properties(${name} PROPERTIES POSITION_INDEPENDENT_CODE ON)
  add_test(NAME ${name} COMMAND ${name})
  set_tests_properties(${name} PROPERTIES TIMEOUT 300)
endfunction()

add_ftl_test(test_address_translation)
add_ftl_test(test_data_buffer)
add_ftl_test(test_garbage_collection)
add_ftl_test(test_request_allocation)
add_ftl_test(test_request_schedule)
add_ftl_test(test_request_transform)
add_ftl_test(test_nvme_admin)
add_ftl_test(test_nvme_io)
add_ftl_test(test_nvme_main)
add_ftl_test(test_harness)

# ---------------------------------------------------------------------------
# Coverage: lcov HTML report + one-line summary of firmware line coverage
# ---------------------------------------------------------------------------
if(FTL_TEST_COVERAGE)
  find_program(LCOV lcov)
  set(COV_DIR ${CMAKE_BINARY_DIR}/coverage)

  add_custom_target(coverage-clean
    COMMAND ${CMAKE_COMMAND} -E rm -rf ${COV_DIR}
    COMMAND ${LCOV} --quiet --directory ${CMAKE_BINARY_DIR} --zerocounters
    COMMENT "Resetting gcov counters")

  # Capture only the firmware sources: the harness, stubs, mocks and Unity are
  # excluded. The glob filtering lives in scripts/coverage.sh so CI and lab
  # machines can run the exact same report outside CMake.
  set(COV_SCRIPT ${CMAKE_CURRENT_SOURCE_DIR}/scripts/coverage.sh)

  add_custom_target(coverage-summary
    COMMAND ${COV_SCRIPT} ${CMAKE_BINARY_DIR} ${FW_DIR} --no-html
    COMMENT "Firmware line coverage summary (${COV_DIR}/summary.txt)")

  add_custom_target(coverage-html
    COMMAND ${COV_SCRIPT} ${CMAKE_BINARY_DIR} ${FW_DIR}
    COMMENT "Firmware line coverage summary + HTML report")

  # Full pipeline: rebuild, reset counters, run every test, report.
  add_custom_target(coverage
    COMMAND ${CMAKE_COMMAND} --build ${CMAKE_BINARY_DIR} --target coverage-clean
    COMMAND ${CMAKE_CTEST_COMMAND} --test-dir ${CMAKE_BINARY_DIR} --output-on-failure
    COMMAND ${CMAKE_COMMAND} --build ${CMAKE_BINARY_DIR} --target coverage-html
    DEPENDS test_address_translation test_data_buffer test_garbage_collection
            test_request_allocation test_request_schedule test_request_transform
            test_nvme_admin test_nvme_io test_nvme_main test_harness
    USES_TERMINAL
    COMMENT "Running tests and producing coverage report")
endif()
