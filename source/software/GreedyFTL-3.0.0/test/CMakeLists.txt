cmake_minimum_required(VERSION 3.16)
project(greedyftl_host_tests C)

# Host-side unit tests for GreedyFTL-3.0.0. See README.md in this directory.
#
#   cmake -S . -B build && cmake --build build && ctest --test-dir build
#   cmake --build build --target coverage      # lcov HTML report + summary line

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS ON)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Debug CACHE STRING "" FORCE)
endif()

option(GREEDYFTL_COVERAGE "Instrument the firmware objects with gcov" ON)

get_filename_component(FW_DIR ${CMAKE_CURRENT_SOURCE_DIR}/.. ABSOLUTE)

include(FetchContent)
FetchContent_Declare(
  unity
  GIT_REPOSITORY https://github.com/ThrowTheSwitch/Unity.git
  GIT_TAG        v2.6.1
  GIT_SHALLOW    TRUE
)
set(UNITY_EXTENSION_FIXTURE OFF CACHE BOOL "" FORCE)
set(UNITY_EXTENSION_MEMORY OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(unity)
target_compile_definitions(unity PUBLIC
  UNITY_INCLUDE_DOUBLE
  UNITY_SUPPORT_64
  UNITY_OUTPUT_COLOR
)

# ---------------------------------------------------------------------------
# Firmware under test (compiled natively, HOST_TEST defined)
# ---------------------------------------------------------------------------
set(FIRMWARE_SOURCES
  ${FW_DIR}/address_translation.c
  ${FW_DIR}/garbage_collection.c
  ${FW_DIR}/data_buffer.c
  ${FW_DIR}/request_allocation.c
  ${FW_DIR}/request_schedule.c
  ${FW_DIR}/request_transform.c
  ${FW_DIR}/ftl_config.c
  ${FW_DIR}/nvme/nvme_admin_cmd.c
  ${FW_DIR}/nvme/nvme_identify.c
  ${FW_DIR}/nvme/nvme_io_cmd.c
  ${FW_DIR}/nvme/nvme_main.c
)

# Firmware sources are third-party style C89/C99 with implicit int conversions
# and unused variables; only host-relevant diagnostics are enabled for them.
set(FIRMWARE_C_FLAGS -Wall -Wno-unused-variable -Wno-unused-but-set-variable
  -Wno-pointer-to-int-cast -Wno-int-to-pointer-cast -Wno-unused-function
  -Wno-implicit-function-declaration -Wno-format -fno-strict-aliasing)

add_library(greedyftl_fw STATIC ${FIRMWARE_SOURCES})
target_compile_definitions(greedyftl_fw PUBLIC HOST_TEST)
target_include_directories(greedyftl_fw PUBLIC
  ${FW_DIR}
  ${FW_DIR}/nvme
  ${CMAKE_CURRENT_SOURCE_DIR}/stubs
  ${CMAKE_CURRENT_SOURCE_DIR}/fakes
)
target_compile_options(greedyftl_fw PRIVATE ${FIRMWARE_C_FLAGS})

if(GREEDYFTL_COVERAGE)
  target_compile_options(greedyftl_fw PRIVATE -O0 -fprofile-arcs -ftest-coverage)
  target_link_options(greedyftl_fw PUBLIC --coverage)
endif()

# ---------------------------------------------------------------------------
# Host stubs and fakes (Xilinx BSP, NAND controller, NVMe host controller)
# ---------------------------------------------------------------------------
add_library(greedyftl_host STATIC
  stubs/xil_printf.c
  fakes/fake_regs.c
  fakes/host_memory.c
  fakes/fake_nsc_driver.c
  fakes/fake_host_lld.c
  support/ftl_test_env.c
)
target_link_libraries(greedyftl_host PUBLIC greedyftl_fw)
target_include_directories(greedyftl_host PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/support)
target_compile_options(greedyftl_host PRIVATE -Wall -Wextra -Wno-pointer-to-int-cast -Wno-int-to-pointer-cast)

# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------
enable_testing()

function(greedyftl_add_test name)
  add_executable(${name} tests/${name}.c)
  target_link_libraries(${name} PRIVATE greedyftl_host unity)
  target_compile_options(${name} PRIVATE -Wall -Wextra -Wno-pointer-to-int-cast -Wno-int-to-pointer-cast)
  add_test(NAME ${name} COMMAND ${name})
endfunction()

greedyftl_add_test(test_address_translation)
greedyftl_add_test(test_garbage_collection)
greedyftl_add_test(test_data_buffer)
greedyftl_add_test(test_request_allocation)
greedyftl_add_test(test_nvme_cmd)

# ---------------------------------------------------------------------------
# Coverage: `cmake --build build --target coverage`
# ---------------------------------------------------------------------------
if(GREEDYFTL_COVERAGE)
  find_program(LCOV lcov)
  find_program(GENHTML genhtml)
  if(LCOV AND GENHTML)
    set(COVERAGE_DIR ${CMAKE_BINARY_DIR}/coverage)
    add_custom_target(coverage
      COMMAND ${LCOV} --zerocounters --directory ${CMAKE_BINARY_DIR} --quiet
      COMMAND ${CMAKE_CTEST_COMMAND} --output-on-failure
      COMMAND ${LCOV} --capture --directory ${CMAKE_BINARY_DIR}
              --base-directory ${FW_DIR}
              --output-file ${COVERAGE_DIR}.raw.info --quiet
              --rc lcov_branch_coverage=1
      # Keep only production firmware sources (test/ fakes are built without
      # instrumentation, so they never appear in the raw capture).
      COMMAND ${LCOV} --extract ${COVERAGE_DIR}.raw.info "${FW_DIR}/*"
              --output-file ${COVERAGE_DIR}.info --quiet
              --rc lcov_branch_coverage=1
      COMMAND ${GENHTML} ${COVERAGE_DIR}.info --output-directory ${COVERAGE_DIR}
              --title "GreedyFTL-3.0.0 host tests" --quiet --branch-coverage
      COMMAND ${CMAKE_COMMAND} -E echo "==== lcov summary (production firmware only) ===="
      COMMAND ${LCOV} --list ${COVERAGE_DIR}.info --rc lcov_branch_coverage=1
      COMMAND ${LCOV} --summary ${COVERAGE_DIR}.info
      COMMAND ${CMAKE_COMMAND} -E echo "HTML report: ${COVERAGE_DIR}/index.html"
      WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
      DEPENDS test_address_translation test_garbage_collection test_data_buffer
              test_request_allocation test_nvme_cmd
      VERBATIM
    )
  else()
    message(WARNING "lcov/genhtml not found; `coverage` target unavailable")
  endif()
endif()
