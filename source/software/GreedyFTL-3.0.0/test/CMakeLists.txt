cmake_minimum_required(VERSION 3.16)
project(greedyftl_host_tests C)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_EXTENSIONS ON)
if(NOT CMAKE_BUILD_TYPE)
	set(CMAKE_BUILD_TYPE Debug CACHE STRING "" FORCE)
endif()

option(GREEDYFTL_COVERAGE "Instrument the firmware objects with gcov" ON)

get_filename_component(FW_DIR ${CMAKE_CURRENT_SOURCE_DIR}/.. ABSOLUTE)
set(TEST_DIR ${CMAKE_CURRENT_SOURCE_DIR})

# ---------------------------------------------------------------------------
# Unity (pinned tagged release)
# ---------------------------------------------------------------------------
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

# ---------------------------------------------------------------------------
# Firmware modules compiled for the host
# ---------------------------------------------------------------------------
set(FIRMWARE_SOURCES
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

# Host substitutes: Xilinx BSP stubs, fake register map, fake NAND controller, DRAM arena.
set(HOST_SUPPORT_SOURCES
	${TEST_DIR}/stubs/xil_stubs.c
	${TEST_DIR}/fakes/fake_regs.c
	${TEST_DIR}/fakes/host_mem.c
	${TEST_DIR}/fakes/nsc_driver.c
)

add_library(greedyftl_fw STATIC ${FIRMWARE_SOURCES} ${HOST_SUPPORT_SOURCES})
target_include_directories(greedyftl_fw PUBLIC
	${TEST_DIR}/stubs
	${TEST_DIR}/fakes
	${FW_DIR}
	${FW_DIR}/nvme)
target_compile_definitions(greedyftl_fw PUBLIC HOST_TEST=1)
# The firmware stores pointers in unsigned int registers; on a 64-bit host that is only
# valid because the arena is mapped below 4 GiB, so silence the width warnings.
target_compile_options(greedyftl_fw PRIVATE
	-Wall -Wno-unused-variable -Wno-unused-but-set-variable -Wno-pointer-to-int-cast
	-Wno-int-to-pointer-cast -Wno-unused-function -Wno-parentheses -Wno-misleading-indentation
	-Wno-return-type -Wno-sign-compare -Wno-format -Wno-maybe-uninitialized -Wno-uninitialized
	-fno-strict-aliasing)

if(GREEDYFTL_COVERAGE)
	target_compile_options(greedyftl_fw PRIVATE -O0 -fprofile-arcs -ftest-coverage)
	target_link_options(greedyftl_fw PUBLIC -fprofile-arcs -ftest-coverage)
	target_link_libraries(greedyftl_fw PUBLIC gcov)
endif()

# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------
enable_testing()

add_library(greedyftl_test_support STATIC ${TEST_DIR}/tests/test_support.c)
target_link_libraries(greedyftl_test_support PUBLIC greedyftl_fw unity)
target_include_directories(greedyftl_test_support PUBLIC ${TEST_DIR}/tests)

function(greedyftl_add_test name)
	add_executable(${name} ${TEST_DIR}/tests/${name}.c)
	target_link_libraries(${name} PRIVATE greedyftl_test_support)
	target_compile_options(${name} PRIVATE -Wall -Wextra -Wno-unused-parameter)
	add_test(NAME ${name} COMMAND ${name})
	set_tests_properties(${name} PROPERTIES TIMEOUT 600)
endfunction()

greedyftl_add_test(test_address_translation)
greedyftl_add_test(test_garbage_collection)
greedyftl_add_test(test_data_buffer)
greedyftl_add_test(test_request_allocation)
greedyftl_add_test(test_request_schedule)
greedyftl_add_test(test_nvme_cmd)

# ---------------------------------------------------------------------------
# Coverage: `cmake --build <build> --target coverage`
# ---------------------------------------------------------------------------
if(GREEDYFTL_COVERAGE)
	find_program(LCOV_EXECUTABLE lcov)
	find_program(GENHTML_EXECUTABLE genhtml)

	if(LCOV_EXECUTABLE AND GENHTML_EXECUTABLE)
		set(COVERAGE_DIR ${CMAKE_BINARY_DIR}/coverage)
		set(COVERAGE_INFO ${COVERAGE_DIR}/greedyftl.info)
		set(COVERAGE_SUMMARY ${COVERAGE_DIR}/summary.txt)
		set(COVERAGE_HTML ${COVERAGE_DIR}/html)

		# lcov 2.x is strict about gcov version mismatches and unexecuted blocks; the
		# errors-to-ignore list is accepted by both 1.x and 2.x.
		execute_process(COMMAND ${LCOV_EXECUTABLE} --version OUTPUT_VARIABLE LCOV_VERSION_OUTPUT ERROR_QUIET)
		if(LCOV_VERSION_OUTPUT MATCHES "version 2")
			set(LCOV_IGNORE --ignore-errors mismatch,negative,unused,inconsistent,gcov,empty)
		else()
			set(LCOV_IGNORE)
		endif()
		string(JOIN " " LCOV_IGNORE_STR ${LCOV_IGNORE})

		add_custom_target(coverage
			COMMAND ${CMAKE_COMMAND} -E make_directory ${COVERAGE_DIR}
			COMMAND ${LCOV_EXECUTABLE} ${LCOV_IGNORE} --zerocounters --directory ${CMAKE_BINARY_DIR}
			COMMAND ${CMAKE_CTEST_COMMAND} --test-dir ${CMAKE_BINARY_DIR} --output-on-failure
			COMMAND ${LCOV_EXECUTABLE} ${LCOV_IGNORE} --capture --directory ${CMAKE_BINARY_DIR}
				--base-directory ${FW_DIR} --rc lcov_branch_coverage=1 --output-file ${COVERAGE_INFO}.all
			COMMAND ${LCOV_EXECUTABLE} ${LCOV_IGNORE} --extract ${COVERAGE_INFO}.all
				"${FW_DIR}/*.c" "${FW_DIR}/nvme/*.c" --output-file ${COVERAGE_INFO}.fw
			COMMAND ${LCOV_EXECUTABLE} ${LCOV_IGNORE} --remove ${COVERAGE_INFO}.fw
				"${TEST_DIR}/*" --output-file ${COVERAGE_INFO}
			COMMAND ${GENHTML_EXECUTABLE} ${LCOV_IGNORE} --output-directory ${COVERAGE_HTML}
				--title "GreedyFTL-3.0.0 host tests" --legend ${COVERAGE_INFO}
			COMMAND sh -c "${LCOV_EXECUTABLE} ${LCOV_IGNORE_STR} --list ${COVERAGE_INFO} | tee ${COVERAGE_SUMMARY}"
			COMMAND sh -c "${LCOV_EXECUTABLE} ${LCOV_IGNORE_STR} --summary ${COVERAGE_INFO} 2>&1 | tee -a ${COVERAGE_SUMMARY}"
			WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
			COMMENT "Running tests and generating lcov report in ${COVERAGE_HTML}"
			VERBATIM)
		add_dependencies(coverage
			test_address_translation test_garbage_collection test_data_buffer
			test_request_allocation test_request_schedule test_nvme_cmd)
	else()
		message(WARNING "lcov/genhtml not found; the 'coverage' target is unavailable")
	endif()
endif()
