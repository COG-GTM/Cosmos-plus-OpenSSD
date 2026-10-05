cmake_minimum_required(VERSION 3.16)
project(greedyftl_host_tests C)

include(FetchContent)
FetchContent_Declare(
    unity
    URL https://github.com/ThrowTheSwitch/Unity/archive/refs/tags/v2.6.0.tar.gz
    URL_HASH SHA256=aa4c9fb1ae5fc5242f914c65f3557e817e40cb37f04a31e5ff76d1ab89dbf674
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)
FetchContent_MakeAvailable(unity)

option(ENABLE_COVERAGE "Compile host tests with GCC coverage instrumentation" ON)
option(GREEDYFTL_RUN_KNOWN_BUGS "Run tests that are expected to expose firmware bugs" OFF)
set(GCOV_TOOL gcov CACHE STRING "gcov executable used by lcov")

get_filename_component(FIRMWARE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/.." ABSOLUTE)
set(HOST_INCLUDE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/stubs")

set(FIRMWARE_SOURCES
    "${FIRMWARE_DIR}/address_translation.c"
    "${FIRMWARE_DIR}/garbage_collection.c"
    "${FIRMWARE_DIR}/data_buffer.c"
    "${FIRMWARE_DIR}/request_allocation.c"
    "${FIRMWARE_DIR}/request_schedule.c"
    "${FIRMWARE_DIR}/request_transform.c"
    "${FIRMWARE_DIR}/ftl_config.c"
    "${FIRMWARE_DIR}/nvme/host_lld.c"
    "${FIRMWARE_DIR}/nvme/nvme_admin_cmd.c"
    "${FIRMWARE_DIR}/nvme/nvme_identify.c"
    "${FIRMWARE_DIR}/nvme/nvme_io_cmd.c"
)
set(HOST_FAKES
    "${CMAKE_CURRENT_SOURCE_DIR}/fakes/host_dram.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/fakes/host_reg_map.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/fakes/fake_nand.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/fakes/fake_host_dma.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/fakes/bsp_stubs.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/fakes/nvme_context_stub.c"
)

add_library(greedyftl_host STATIC ${FIRMWARE_SOURCES} ${HOST_FAKES})
target_include_directories(greedyftl_host BEFORE PUBLIC
    "${HOST_INCLUDE_DIR}"
    "${FIRMWARE_DIR}"
    "${FIRMWARE_DIR}/nvme"
)
target_compile_definitions(greedyftl_host PUBLIC HOST_TEST)
target_compile_options(greedyftl_host PRIVATE -std=gnu99 -O0 -g)
target_compile_options(greedyftl_host PUBLIC -include host_memory_map_limits.h)

if(ENABLE_COVERAGE)
    target_compile_options(greedyftl_host PRIVATE -fprofile-arcs -ftest-coverage)
    target_link_options(greedyftl_host PUBLIC -fprofile-arcs -ftest-coverage)
endif()

if(GREEDYFTL_RUN_KNOWN_BUGS)
    target_compile_definitions(greedyftl_host PUBLIC RUN_KNOWN_BUGS)
endif()

enable_testing()
file(GLOB TEST_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/unit/test_*.c")
foreach(TEST_SOURCE IN LISTS TEST_SOURCES)
    get_filename_component(TEST_NAME "${TEST_SOURCE}" NAME_WE)
    add_executable("${TEST_NAME}" "${TEST_SOURCE}" "${CMAKE_CURRENT_SOURCE_DIR}/unit/ftl_fixture.c")
    target_link_libraries("${TEST_NAME}" PRIVATE greedyftl_host unity)
    target_include_directories("${TEST_NAME}" PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/unit")
    target_compile_options("${TEST_NAME}" PRIVATE -std=gnu99 -O0 -g)
    if(ENABLE_COVERAGE)
        target_compile_options("${TEST_NAME}" PRIVATE -fprofile-arcs -ftest-coverage)
        target_link_options("${TEST_NAME}" PRIVATE -fprofile-arcs -ftest-coverage)
    endif()
    add_test(NAME "${TEST_NAME}" COMMAND "${TEST_NAME}")
    list(APPEND TEST_TARGETS "${TEST_NAME}")
endforeach()

if(ENABLE_COVERAGE)
    find_program(LCOV_EXECUTABLE lcov REQUIRED)
    find_program(GENHTML_EXECUTABLE genhtml REQUIRED)
    find_package(Python3 REQUIRED COMPONENTS Interpreter)
    add_custom_target(coverage
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${CMAKE_BINARY_DIR}/coverage"
        COMMAND "${LCOV_EXECUTABLE}" --zerocounters --directory "${CMAKE_BINARY_DIR}"
        COMMAND "${LCOV_EXECUTABLE}" --capture --initial --directory "${CMAKE_BINARY_DIR}"
                --gcov-tool "${GCOV_TOOL}" --output-file "${CMAKE_BINARY_DIR}/coverage/base.info"
        COMMAND "${CMAKE_CTEST_COMMAND}" --output-on-failure
        COMMAND "${LCOV_EXECUTABLE}" --capture --directory "${CMAKE_BINARY_DIR}"
                --gcov-tool "${GCOV_TOOL}" --output-file "${CMAKE_BINARY_DIR}/coverage/test.info"
        COMMAND "${LCOV_EXECUTABLE}" --add-tracefile "${CMAKE_BINARY_DIR}/coverage/base.info"
                --add-tracefile "${CMAKE_BINARY_DIR}/coverage/test.info"
                --output-file "${CMAKE_BINARY_DIR}/coverage/all.info"
        COMMAND "${LCOV_EXECUTABLE}" --extract "${CMAKE_BINARY_DIR}/coverage/all.info"
                ${FIRMWARE_SOURCES}
                --output-file "${CMAKE_BINARY_DIR}/coverage/firmware.info"
        COMMAND "${GENHTML_EXECUTABLE}" "${CMAKE_BINARY_DIR}/coverage/firmware.info"
                --output-directory "${CMAKE_BINARY_DIR}/coverage/html"
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/coverage_summary.py"
                "${LCOV_EXECUTABLE}" "${CMAKE_BINARY_DIR}/coverage/firmware.info"
                "${CMAKE_BINARY_DIR}/coverage/summary.txt"
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/coverage_table.py"
                "${CMAKE_BINARY_DIR}/coverage/firmware.info"
                "${CMAKE_BINARY_DIR}/coverage/per_file.md"
        COMMAND "${CMAKE_COMMAND}" -E cat "${CMAKE_BINARY_DIR}/coverage/summary.txt"
        DEPENDS ${TEST_SOURCES} "${CMAKE_CURRENT_SOURCE_DIR}/coverage_table.py"
                "${CMAKE_CURRENT_SOURCE_DIR}/coverage_summary.py" ${TEST_TARGETS}
        USES_TERMINAL
        VERBATIM
    )
endif()
