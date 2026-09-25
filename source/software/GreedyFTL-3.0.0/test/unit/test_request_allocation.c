/* Smoke test for request_allocation.c: the free request queue hands out
 * every slot exactly once and takes them back. */
#include "unity.h"
#include "ftl_test_env.h"
#include "request_allocation.h"

void setUp(void) { ftl_test_env_reset(); InitReqPool(); }
void tearDown(void) {}

static void test_free_queue_round_trip(void)
{
	unsigned int a, b;

	TEST_ASSERT_EQUAL_UINT(AVAILABLE_OUNTSTANDING_REQ_COUNT, freeReqQ.reqCnt);
	a = GetFromFreeReqQ();
	b = GetFromFreeReqQ();
	TEST_ASSERT_NOT_EQUAL(a, b);
	TEST_ASSERT_EQUAL_UINT(AVAILABLE_OUNTSTANDING_REQ_COUNT - 2, freeReqQ.reqCnt);
	PutToFreeReqQ(a);
	PutToFreeReqQ(b);
	TEST_ASSERT_EQUAL_UINT(AVAILABLE_OUNTSTANDING_REQ_COUNT, freeReqQ.reqCnt);
	TEST_ASSERT_EQUAL_UINT(b, freeReqQ.tailReq);
}

int main(void)
{
	UNITY_BEGIN();
	RUN_TEST(test_free_queue_round_trip);
	return UNITY_END();
}
