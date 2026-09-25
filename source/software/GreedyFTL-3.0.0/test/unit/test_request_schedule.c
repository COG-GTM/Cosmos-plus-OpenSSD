/* Smoke test for request_schedule.c: after boot every die is idle and sits
 * on its channel's idle list. */
#include "unity.h"
#include "ftl_test_env.h"
#include "request_allocation.h"
#include "request_schedule.h"

/* request_schedule.h declares dieStatusTablePtr but the definition is dieStateTablePtr. */
extern P_DIE_STATE_TABLE dieStateTablePtr;

void setUp(void) { ftl_test_env_reset(); ftl_test_env_init_ftl(); }
void tearDown(void) {}

static void test_all_dies_idle_after_boot(void)
{
	unsigned int ch, way;

	for (ch = 0; ch < USER_CHANNELS; ch++) {
		TEST_ASSERT_NOT_EQUAL(WAY_NONE, wayPriorityTablePtr->wayPriority[ch].idleHead);
		TEST_ASSERT_EQUAL_UINT(WAY_NONE, wayPriorityTablePtr->wayPriority[ch].statusReportHead);
		for (way = 0; way < USER_WAYS; way++) {
			TEST_ASSERT_EQUAL_UINT(DIE_STATE_IDLE, dieStateTablePtr->dieState[ch][way].dieState);
			TEST_ASSERT_EQUAL_UINT(REQ_STATUS_CHECK_OPT_NONE, dieStateTablePtr->dieState[ch][way].reqStatusCheckOpt);
		}
	}
	TEST_ASSERT_EQUAL_UINT(0, notCompletedNandReqCnt);
}

int main(void)
{
	UNITY_BEGIN();
	RUN_TEST(test_all_dies_idle_after_boot);
	return UNITY_END();
}
