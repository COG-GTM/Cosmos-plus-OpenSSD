cmake_minimum_required(VERSION 3.16)
project(greedyftl_host_tests C)

set(FTL_DIR ${CMAKE_CURRENT_SOURCE_DIR}/..)

# The firmware's assert() must not abort on the host so that error-return paths are testable.
set(FTL_HOST_FLAGS -DHOST_TEST -DNDEBUG -fprofile-arcs -ftest-coverage -O0 -g -w)

add_library(ftl_stubs STATIC stubs/fake_regs.c stubs/ftl_stubs.c)
target_include_directories(ftl_stubs PUBLIC stubs ${FTL_DIR})
target_compile_options(ftl_stubs PRIVATE -DHOST_TEST -O0 -g -Wall)

add_library(ftl_under_test STATIC ${FTL_DIR}/garbage_collection.c)
target_include_directories(ftl_under_test PUBLIC stubs ${FTL_DIR})
target_compile_options(ftl_under_test PRIVATE ${FTL_HOST_FLAGS})
target_link_options(ftl_under_test PUBLIC --coverage)

enable_testing()

add_executable(test_garbage_collection test_garbage_collection.c)
target_link_libraries(test_garbage_collection ftl_under_test ftl_stubs)
target_compile_options(test_garbage_collection PRIVATE -DHOST_TEST -O0 -g -Wall)
add_test(NAME garbage_collection COMMAND test_garbage_collection)

# `make coverage` runs the tests and reports per-file line coverage
# (lcov/genhtml when available, plain gcov otherwise).
add_custom_target(coverage
	COMMAND ${CMAKE_CTEST_COMMAND} --output-on-failure
	COMMAND ${CMAKE_COMMAND} -E env FTL_DIR=${FTL_DIR} BUILD_DIR=${CMAKE_BINARY_DIR}
	        sh ${CMAKE_CURRENT_SOURCE_DIR}/coverage.sh
	WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
	DEPENDS test_garbage_collection)
