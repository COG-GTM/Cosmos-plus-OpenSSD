cmake_minimum_required(VERSION 3.16)
project(greedyftl_host_tests C)

# Host-side unit tests for the GreedyFTL-3.0.0 firmware. The firmware's pure-logic
# modules are compiled natively with -DHOST_TEST; Xilinx BSP headers and the NAND /
# NVMe hardware are replaced by the stand-ins in stubs/ and fakes/.

option(FTL_COVERAGE "Instrument the firmware objects for gcov/lcov" ON)
set(HOST_TEST_CHANNELS 2 CACHE STRING "NAND channels exposed by the host model (1-8)")

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_EXTENSIONS ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Debug CACHE STRING "" FORCE)
endif()

get_filename_component(FW_DIR ${CMAKE_CURRENT_SOURCE_DIR}/.. ABSOLUTE)

include(FetchContent)
FetchContent_Declare(
  unity
  GIT_REPOSITORY https://github.com/ThrowTheSwitch/Unity.git
  GIT_TAG v2.6.0
  GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(unity)

# --- Firmware under test -----------------------------------------------------------
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

add_library(greedyftl_fw OBJECT ${FW_SOURCES})
target_include_directories(greedyftl_fw PUBLIC
  ${FW_DIR}
  ${CMAKE_CURRENT_SOURCE_DIR}/stubs
  ${CMAKE_CURRENT_SOURCE_DIR}/fakes
)
target_compile_definitions(greedyftl_fw PUBLIC HOST_TEST HOST_TEST_CHANNELS=${HOST_TEST_CHANNELS})
# The firmware casts 32-bit DRAM addresses to pointers; that is intentional and
# supported by the sub-4GiB host memory image, so silence only those warnings.
target_compile_options(greedyftl_fw PRIVATE
  -Wall -Wno-int-to-pointer-cast -Wno-pointer-to-int-cast -Wno-unused-variable
  -Wno-unused-but-set-variable -Wno-unused-function -Wno-parentheses -Wno-misleading-indentation
)
if(FTL_COVERAGE)
  target_compile_options(greedyftl_fw PRIVATE -O0 -g -fprofile-arcs -ftest-coverage)
  target_link_options(greedyftl_fw PUBLIC --coverage)
endif()

# --- Host support: BSP stubs + hardware fakes -------------------------------------
add_library(greedyftl_host_support OBJECT
  stubs/xil_stubs.c
  stubs/fake_regs.c
  stubs/host_memory.c
  fakes/fake_nand.c
  fakes/fake_nsc_driver.c
  fakes/fake_host_dma.c
  tests/test_support.c
)
target_link_libraries(greedyftl_host_support PUBLIC greedyftl_fw unity)

# Firmware and fakes reference each other (V2F*/hostDramBase vs chCtlReg), so both
# object sets are combined into one library to keep the link order-independent.
add_library(greedyftl_host STATIC
  $<TARGET_OBJECTS:greedyftl_fw>
  $<TARGET_OBJECTS:greedyftl_host_support>
)
target_link_libraries(greedyftl_host PUBLIC greedyftl_fw greedyftl_host_support)
target_compile_options(greedyftl_host_support PRIVATE -Wall -Wextra -Wno-int-to-pointer-cast -Wno-pointer-to-int-cast)

# --- Tests ------------------------------------------------------------------------
enable_testing()

function(ftl_add_test name)
  add_executable(${name} tests/${name}.c)
  target_link_libraries(${name} PRIVATE greedyftl_host)
  target_compile_options(${name} PRIVATE -Wall -Wextra -Wno-int-to-pointer-cast -Wno-pointer-to-int-cast)
  add_test(NAME ${name} COMMAND ${name})
endfunction()

ftl_add_test(test_address_translation)
ftl_add_test(test_garbage_collection)
ftl_add_test(test_data_buffer)
ftl_add_test(test_request_allocation)
ftl_add_test(test_nvme)

# --- Coverage ---------------------------------------------------------------------
if(FTL_COVERAGE)
  find_program(LCOV_EXECUTABLE lcov)
  find_program(GENHTML_EXECUTABLE genhtml)
  if(LCOV_EXECUTABLE AND GENHTML_EXECUTABLE)
    set(COVERAGE_DIR ${CMAKE_BINARY_DIR}/coverage)
    add_custom_target(coverage
      COMMAND ${CMAKE_COMMAND} -E make_directory ${COVERAGE_DIR}
      COMMAND ${LCOV_EXECUTABLE} --zerocounters --directory ${CMAKE_BINARY_DIR} --quiet
      COMMAND ${CMAKE_CTEST_COMMAND} --output-on-failure
      COMMAND ${LCOV_EXECUTABLE} --capture --directory ${CMAKE_BINARY_DIR}
              --rc lcov_branch_coverage=1
              --output-file ${COVERAGE_DIR}/coverage.raw.info --quiet
      COMMAND ${LCOV_EXECUTABLE} --extract ${COVERAGE_DIR}/coverage.raw.info
              "${FW_DIR}/*" --rc lcov_branch_coverage=1
              --output-file ${COVERAGE_DIR}/coverage.info --quiet
      COMMAND ${GENHTML_EXECUTABLE} ${COVERAGE_DIR}/coverage.info --output-directory ${COVERAGE_DIR}/html
              --branch-coverage --title "GreedyFTL-3.0.0 host tests" --quiet
      COMMAND ${LCOV_EXECUTABLE} --list ${COVERAGE_DIR}/coverage.info
      COMMAND ${LCOV_EXECUTABLE} --summary ${COVERAGE_DIR}/coverage.info
      WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
      COMMENT "Running tests and generating lcov HTML report in ${COVERAGE_DIR}/html"
      VERBATIM
    )
  else()
    message(WARNING "lcov/genhtml not found; the 'coverage' target is unavailable")
  endif()
endif()
