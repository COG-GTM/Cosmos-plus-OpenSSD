/* Host-side replacement for <assert.h>.
 *
 * The firmware uses assert(!"message") for unrecoverable configuration/logic
 * errors. On the board that halts the CPU; under test it is routed to the
 * harness hook so a test can expect an assertion (via setjmp/longjmp) instead
 * of aborting the whole test binary. */
#ifndef GREEDYFTL_TEST_ASSERT_H
#define GREEDYFTL_TEST_ASSERT_H
#ifdef __cplusplus
extern "C" {
#endif
void ftl_test_assert_failed(const char *expr, const char *file, int line);
#ifdef __cplusplus
}
#endif
#undef assert
#define assert(expr) ((expr) ? (void)0 : ftl_test_assert_failed(#expr, __FILE__, __LINE__))
#endif
