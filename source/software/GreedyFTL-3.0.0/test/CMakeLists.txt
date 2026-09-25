cmake_minimum_required(VERSION 3.16)

if(POLICY CMP0135)
  cmake_policy(SET CMP0135 NEW)
endif()

project(GreedyFTLHostTests LANGUAGES C)

# ----------------------------------------------------------------------------
# Options
# ----------------------------------------------------------------------------
set(GREEDYFTL_TEST_CHANNELS 2 CACHE STRING
    "Number of NAND channels exposed by the stub xparameters.h (1..8)")
option(GREEDYFTL_COVERAGE "Instrument firmware sources with gcov" ON)

if(NOT GREEDYFTL_TEST_CHANNELS MATCHES "^[1-8]$")
  message(FATAL_ERROR "GREEDYFTL_TEST_CHANNELS must be 1..8, got '${GREEDYFTL_TEST_CHANNELS}'")
endif()

if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
  message(FATAL_ERROR "The GreedyFTL host tests require a 64-bit Linux host")
endif()

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Debug CACHE STRING "Build type" FORCE)
endif()

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_EXTENSIONS ON)
# The firmware DRAM window (fw_memory.c) is mapped at 0x00100000-0x3FFFFFFF;
# a PIE executable guarantees the loader keeps that range free.
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

get_filename_component(FW_DIR "${CMAKE_CURRENT_SOURCE_DIR}/.." ABSOLUTE)
set(TEST_DIR "${CMAKE_CURRENT_SOURCE_DIR}")

# ----------------------------------------------------------------------------
# Unity (pinned release tarball, verified by hash)
# ----------------------------------------------------------------------------
include(FetchContent)
set(GREEDYFTL_UNITY_VERSION 2.6.0)
FetchContent_Declare(unity
  URL https://github.com/ThrowTheSwitch/Unity/archive/refs/tags/v${GREEDYFTL_UNITY_VERSION}.tar.gz
  URL_HASH SHA256=aa4c9fb1ae5fc5242f914c65f3557e817e40cb37f04a31e5ff76d1ab89dbf674)
FetchContent_MakeAvailable(unity)

# ----------------------------------------------------------------------------
# Firmware under test
# ----------------------------------------------------------------------------
# main.c (Zynq MMU/GIC bring-up) and nsc_driver.c / nvme/host_lld.c (MMIO
# drivers) are replaced by mocks; everything else is compiled unmodified.
set(GREEDYFTL_FW_SOURCES
  ${FW_DIR}/address_translation.c
  ${FW_DIR}/data_buffer.c
  ${FW_DIR}/ftl_config.c
  ${FW_DIR}/garbage_collection.c
  ${FW_DIR}/request_allocation.c
  ${FW_DIR}/request_schedule.c
  ${FW_DIR}/request_transform.c
  ${FW_DIR}/nvme/nvme_admin_cmd.c
  ${FW_DIR}/nvme/nvme_identify.c
  ${FW_DIR}/nvme/nvme_io_cmd.c
  ${FW_DIR}/nvme/nvme_main.c)

# Include order matters: stubs/ first so <assert.h> and the Xilinx BSP
# headers resolve to the host stubs.
set(GREEDYFTL_INCLUDE_DIRS
  ${TEST_DIR}/stubs
  ${TEST_DIR}/mocks
  ${FW_DIR})

set(GREEDYFTL_COMMON_DEFS GREEDYFTL_HOST_TEST=1 GREEDYFTL_TEST_CHANNELS=${GREEDYFTL_TEST_CHANNELS})

# Legacy firmware idioms (implicit int<->pointer casts, K&R prototypes) are
# expected; keep the log readable but still flag real problems.
set(GREEDYFTL_FW_WARNINGS
  -Wall
  -Wno-int-to-pointer-cast
  -Wno-pointer-to-int-cast
  -Wno-unused-variable
  -Wno-unused-but-set-variable
  -Wno-parentheses
  -Wno-misleading-indentation
  -Wno-dangling-else)

set(GREEDYFTL_COVERAGE_FLAGS --coverage -O0 -fno-inline)

function(greedyftl_configure_fw_target target)
  target_include_directories(${target} PRIVATE ${GREEDYFTL_INCLUDE_DIRS})
  target_compile_definitions(${target} PRIVATE ${GREEDYFTL_COMMON_DEFS})
  target_compile_options(${target} PRIVATE ${GREEDYFTL_FW_WARNINGS})
  if(GREEDYFTL_COVERAGE)
    target_compile_options(${target} PRIVATE ${GREEDYFTL_COVERAGE_FLAGS})
  endif()
endfunction()

add_library(greedyftl_fw STATIC ${GREEDYFTL_FW_SOURCES})
greedyftl_configure_fw_target(greedyftl_fw)

# Real NVMe host LLD, built only for the register-capture harness test.
add_library(greedyftl_host_lld STATIC ${FW_DIR}/nvme/host_lld.c)
greedyftl_configure_fw_target(greedyftl_host_lld)

# ----------------------------------------------------------------------------
# Stubs and mocks (not instrumented)
# ----------------------------------------------------------------------------
add_library(greedyftl_stubs STATIC
  stubs/xil_printf.c
  stubs/xtime_l.c
  mocks/fw_assert.c
  mocks/fw_memory.c
  mocks/mock_io.c)
target_include_directories(greedyftl_stubs PUBLIC ${GREEDYFTL_INCLUDE_DIRS})
target_compile_definitions(greedyftl_stubs PUBLIC ${GREEDYFTL_COMMON_DEFS})
# Every target that links the stubs (firmware, mocks, tests) sees the same
# IO_READ32/IO_WRITE32/ASSERT redirection.
target_compile_options(greedyftl_stubs PUBLIC -include ${TEST_DIR}/stubs/greedyftl_test_prelude.h)
target_compile_options(greedyftl_stubs PRIVATE -Wall -Wextra)
target_link_libraries(greedyftl_stubs PUBLIC unity)

add_library(greedyftl_mocks STATIC
  mocks/mock_nsc.c
  mocks/mock_host_lld.c
  mocks/fw_test.c)
target_compile_options(greedyftl_mocks PRIVATE -Wall -Wextra)
target_link_libraries(greedyftl_mocks PUBLIC greedyftl_stubs)

# Firmware and mocks reference each other; CMake repeats cyclic static libs.
target_link_libraries(greedyftl_fw PUBLIC greedyftl_mocks)
target_link_libraries(greedyftl_mocks PUBLIC greedyftl_fw)
target_link_libraries(greedyftl_host_lld PUBLIC greedyftl_stubs)

if(GREEDYFTL_COVERAGE)
  target_link_options(greedyftl_fw INTERFACE --coverage)
  target_link_options(greedyftl_host_lld INTERFACE --coverage)
endif()

# ----------------------------------------------------------------------------
# Tests
# ----------------------------------------------------------------------------
enable_testing()

# greedyftl_add_test(<name> SOURCES <files...> [LIBS <libs...>] [WRAP <symbols...>])
# WRAP links with -Wl,--wrap=<symbol> so a test can replace a firmware
# function called from another translation unit by defining __wrap_<symbol>
# (the original stays reachable as __real_<symbol>).
function(greedyftl_add_test name)
  cmake_parse_arguments(ARG "" "" "SOURCES;LIBS;WRAP" ${ARGN})
  add_executable(${name} ${ARG_SOURCES})
  target_compile_options(${name} PRIVATE -Wall -Wextra -Wno-unused-parameter)
  if(ARG_LIBS)
    target_link_libraries(${name} PRIVATE ${ARG_LIBS})
  else()
    target_link_libraries(${name} PRIVATE greedyftl_fw greedyftl_mocks)
  endif()
  foreach(symbol IN LISTS ARG_WRAP)
    target_link_options(${name} PRIVATE "LINKER:--wrap=${symbol}")
  endforeach()
  add_test(NAME ${name} COMMAND ${name})
  set_tests_properties(${name} PROPERTIES TIMEOUT 300)
endfunction()

# Every unit/test_*.c is its own executable so module owners never need to
# edit this file. An optional unit/test_<x>.wrap lists symbols (one per line)
# to pass to --wrap for that test.
file(GLOB GREEDYFTL_UNIT_TESTS CONFIGURE_DEPENDS ${TEST_DIR}/unit/test_*.c)
foreach(test_source IN LISTS GREEDYFTL_UNIT_TESTS)
  get_filename_component(test_name ${test_source} NAME_WE)
  set(wrap_file ${TEST_DIR}/unit/${test_name}.wrap)
  set(wrap_symbols "")
  if(EXISTS ${wrap_file})
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${wrap_file})
    file(STRINGS ${wrap_file} wrap_symbols REGEX "^[A-Za-z_][A-Za-z0-9_]*$")
  endif()
  greedyftl_add_test(${test_name} SOURCES ${test_source} WRAP ${wrap_symbols})
endforeach()

greedyftl_add_test(test_harness_mocks SOURCES harness/test_harness_mocks.c)
greedyftl_add_test(test_harness_host_lld
  SOURCES harness/test_harness_host_lld.c
  LIBS greedyftl_host_lld greedyftl_stubs)

# ----------------------------------------------------------------------------
# Coverage: `cmake --build <dir> --target coverage`
# ----------------------------------------------------------------------------
if(GREEDYFTL_COVERAGE)
  add_custom_target(coverage
    COMMAND ${TEST_DIR}/scripts/coverage.sh ${CMAKE_BINARY_DIR} ${FW_DIR}
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
    USES_TERMINAL
    COMMENT "Running tests and generating lcov summary + HTML report")
  add_dependencies(coverage greedyftl_fw greedyftl_host_lld)
  foreach(test_source IN LISTS GREEDYFTL_UNIT_TESTS)
    get_filename_component(test_name ${test_source} NAME_WE)
    add_dependencies(coverage ${test_name})
  endforeach()
  add_dependencies(coverage test_harness_mocks test_harness_host_lld)
endif()
