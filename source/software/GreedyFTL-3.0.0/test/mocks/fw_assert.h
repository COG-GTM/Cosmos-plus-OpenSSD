/*
 * Capture of firmware assert()/ASSERT() failures and escape from firmware
 * infinite loops.
 *
 * On target a failed ASSERT() spins forever and assert() aborts. In tests both
 * call fw_assert_fail(): if a test armed FW_EXPECT_ASSERT the call longjmps
 * back to it, otherwise the current Unity test fails with file:line.
 *
 * fw_loop_exit() lets mocks break out of never-returning firmware loops such
 * as nvme_main(); see FW_RUN_UNTIL_LOOP_EXIT.
 */
#ifndef FW_ASSERT_H
#define FW_ASSERT_H

#include <setjmp.h>

void fw_assert_fail(const char *file, int line, const char *expr) __attribute__((noreturn));
void fw_loop_exit(void) __attribute__((noreturn));

void fw_assert_arm(jmp_buf *target);
void fw_assert_disarm(void);
void fw_loop_exit_arm(jmp_buf *target);
void fw_loop_exit_disarm(void);

unsigned int fw_assert_count(void);
const char *fw_assert_last_file(void);
int fw_assert_last_line(void);
const char *fw_assert_last_expr(void);
void fw_assert_reset(void);

/* Runs stmt and fails the test unless it trips a firmware assert. */
#define FW_EXPECT_ASSERT(stmt)                                                   \
	do {                                                                         \
		jmp_buf fw_expect_assert_jb_;                                            \
		fw_assert_arm(&fw_expect_assert_jb_);                                    \
		if (setjmp(fw_expect_assert_jb_) == 0) {                                 \
			stmt;                                                                \
			fw_assert_disarm();                                                  \
			TEST_FAIL_MESSAGE("expected firmware assert was not raised: " #stmt); \
		}                                                                        \
		fw_assert_disarm();                                                      \
	} while (0)

/* Runs a never-returning firmware loop until a mock calls fw_loop_exit(). */
#define FW_RUN_UNTIL_LOOP_EXIT(stmt)                                             \
	do {                                                                         \
		jmp_buf fw_loop_exit_jb_;                                                \
		fw_loop_exit_arm(&fw_loop_exit_jb_);                                     \
		if (setjmp(fw_loop_exit_jb_) == 0) {                                     \
			stmt;                                                                \
		}                                                                        \
		fw_loop_exit_disarm();                                                   \
	} while (0)

#endif /* FW_ASSERT_H */
