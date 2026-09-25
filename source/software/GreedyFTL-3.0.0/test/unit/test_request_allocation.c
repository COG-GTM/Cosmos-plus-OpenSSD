/* Unit tests for request_allocation.c. */
#include "unity.h"

#include "fw_test.h"
#include "memory_map.h"

void setUp(void)
{
	fw_test_reset();
	InitReqPool();
}

void tearDown(void) {}

static void test_smoke_init_req_pool_puts_every_slot_on_free_queue(void)
{
	TEST_ASSERT_EQUAL_UINT(AVAILABLE_OUNTSTANDING_REQ_COUNT, freeReqQ.reqCnt);
	TEST_ASSERT_EQUAL_UINT(0, freeReqQ.headReq);
	TEST_ASSERT_EQUAL_UINT(AVAILABLE_OUNTSTANDING_REQ_COUNT - 1, freeReqQ.tailReq);
	TEST_ASSERT_EQUAL_PTR(fw_ptr(REQ_POOL_ADDR), reqPoolPtr);
}

static void test_smoke_alloc_then_release_round_trips_slot(void)
{
	unsigned int reqSlotTag = GetFromFreeReqQ();

	TEST_ASSERT_EQUAL_UINT(0, reqSlotTag);
	TEST_ASSERT_EQUAL_UINT(AVAILABLE_OUNTSTANDING_REQ_COUNT - 1, freeReqQ.reqCnt);

	PutToFreeReqQ(reqSlotTag);
	TEST_ASSERT_EQUAL_UINT(AVAILABLE_OUNTSTANDING_REQ_COUNT, freeReqQ.reqCnt);
	TEST_ASSERT_EQUAL_UINT(REQ_QUEUE_TYPE_FREE, reqPoolPtr->reqPool[reqSlotTag].reqQueueType);
}

int main(void)
{
	UNITY_BEGIN();
	RUN_TEST(test_smoke_init_req_pool_puts_every_slot_on_free_queue);
	RUN_TEST(test_smoke_alloc_then_release_round_trips_slot);
	return UNITY_END();
}
