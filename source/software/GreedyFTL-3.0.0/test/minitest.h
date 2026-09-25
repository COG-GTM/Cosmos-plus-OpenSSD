// Minimal Unity-style assertion macros (no external dependency needed on the host).
#ifndef MINITEST_H
#define MINITEST_H

#include <stdio.h>
#include <stdlib.h>

static int mt_tests_run, mt_tests_failed, mt_tests_ignored, mt_current_failed;

#define TEST_FAIL_MESSAGE(msg) \
	do { printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, (msg)); mt_current_failed = 1; return; } while(0)

#define TEST_ASSERT_TRUE(cond) \
	do { if(!(cond)) TEST_FAIL_MESSAGE("expected true: " #cond); } while(0)

#define TEST_ASSERT_FALSE(cond) TEST_ASSERT_TRUE(!(cond))

#define TEST_ASSERT_EQUAL_UINT(expected, actual) \
	do { unsigned long long mt_e = (unsigned long long)(expected), mt_a = (unsigned long long)(actual); \
	     if(mt_e != mt_a) { printf("  FAIL %s:%d: %s expected %llu (0x%llx) was %llu (0x%llx)\n", \
	         __FILE__, __LINE__, #actual, mt_e, mt_e, mt_a, mt_a); mt_current_failed = 1; return; } } while(0)

#define TEST_IGNORE_MESSAGE(msg) \
	do { printf("  IGNORE %s:%d: %s\n", __FILE__, __LINE__, (msg)); mt_tests_ignored++; return; } while(0)

#define RUN_TEST(fn) \
	do { mt_tests_run++; mt_current_failed = 0; setUp(); fn(); tearDown(); \
	     printf("%s %s\n", mt_current_failed ? "FAIL" : "PASS", #fn); \
	     if(mt_current_failed) mt_tests_failed++; } while(0)

#define UNITY_BEGIN() (mt_tests_run = mt_tests_failed = mt_tests_ignored = 0)
#define UNITY_END() \
	(printf("-----------------------\n%d Tests %d Failures %d Ignored\n%s\n", mt_tests_run, mt_tests_failed, \
	        mt_tests_ignored, mt_tests_failed ? "FAIL" : "OK"), mt_tests_failed ? EXIT_FAILURE : EXIT_SUCCESS)

#endif
