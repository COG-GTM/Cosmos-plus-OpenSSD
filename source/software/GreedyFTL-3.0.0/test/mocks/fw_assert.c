#include "fw_assert.h"

#include <stdio.h>
#include <stdlib.h>

#include "unity.h"

static jmp_buf *assert_target;
static jmp_buf *loop_exit_target;
static unsigned int assert_count;
static const char *last_file;
static int last_line;
static const char *last_expr;

void fw_assert_fail(const char *file, int line, const char *expr)
{
	static char message[512];

	assert_count++;
	last_file = file;
	last_line = line;
	last_expr = expr;

	if (assert_target) {
		jmp_buf *target = assert_target;
		assert_target = NULL;
		longjmp(*target, 1);
	}

	snprintf(message, sizeof(message), "unexpected firmware assert at %s:%d: %s", file, line, expr);
	TEST_FAIL_MESSAGE(message);
	abort(); /* TEST_FAIL_MESSAGE longjmps into Unity; never reached. */
}

void fw_loop_exit(void)
{
	jmp_buf *target = loop_exit_target;

	if (!target) {
		TEST_FAIL_MESSAGE("fw_loop_exit() called outside FW_RUN_UNTIL_LOOP_EXIT");
		abort();
	}
	loop_exit_target = NULL;
	longjmp(*target, 1);
}

void fw_assert_arm(jmp_buf *target) { assert_target = target; }
void fw_assert_disarm(void) { assert_target = NULL; }
void fw_loop_exit_arm(jmp_buf *target) { loop_exit_target = target; }
void fw_loop_exit_disarm(void) { loop_exit_target = NULL; }

unsigned int fw_assert_count(void) { return assert_count; }
const char *fw_assert_last_file(void) { return last_file; }
int fw_assert_last_line(void) { return last_line; }
const char *fw_assert_last_expr(void) { return last_expr; }

void fw_assert_reset(void)
{
	assert_target = NULL;
	loop_exit_target = NULL;
	assert_count = 0;
	last_file = NULL;
	last_line = 0;
	last_expr = NULL;
}
