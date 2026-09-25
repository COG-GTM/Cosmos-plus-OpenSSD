/*
 * Replaces <assert.h> for firmware sources so a failed assert() is reported
 * to the test harness (fw_assert.h) instead of aborting the process.
 */
#ifndef GREEDYFTL_TEST_ASSERT_H
#define GREEDYFTL_TEST_ASSERT_H

#include "fw_assert.h"

#undef assert
#define assert(expr) ((expr) ? (void)0 : fw_assert_fail(__FILE__, __LINE__, #expr))

#endif /* GREEDYFTL_TEST_ASSERT_H */
