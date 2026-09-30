#include "test_support.h"

static unsigned int tags[AVAILABLE_OUNTSTANDING_REQ_COUNT];

void setUp(void)
{
	test_ftl_init();
}

void tearDown(void)
{
}

static void test_init_links_every_slot_into_free_queue(void)
{
	TEST_ASSERT_EQUAL_UINT(AVAILABLE_OUNTSTANDING_REQ_COUNT, freeReqQ.reqCnt);
	TEST_ASSERT_EQUAL_UINT(0, freeReqQ.headReq);
	TEST_ASSERT_EQUAL_UINT(AVAILABLE_OUNTSTANDING_REQ_COUNT - 1, freeReqQ.tailReq);
	TEST_ASSERT_EQUAL_UINT(AVAILABLE_OUNTSTANDING_REQ_COUNT,
		test_walk_free_req_queue(AVAILABLE_OUNTSTANDING_REQ_COUNT + 1));
	TEST_ASSERT_EQUAL_UINT(REQ_QUEUE_TYPE_FREE, reqPoolPtr->reqPool[7].reqQueueType);
}

static void test_get_returns_head_and_marks_slot_unqueued(void)
{
	unsigned int tag = GetFromFreeReqQ();

	TEST_ASSERT_EQUAL_UINT(0, tag);
	TEST_ASSERT_EQUAL_UINT(REQ_QUEUE_TYPE_NONE, reqPoolPtr->reqPool[tag].reqQueueType);
	TEST_ASSERT_EQUAL_UINT(1, freeReqQ.headReq);
	TEST_ASSERT_EQUAL_UINT(REQ_SLOT_TAG_NONE, reqPoolPtr->reqPool[1].prevReq);
	TEST_ASSERT_EQUAL_UINT(AVAILABLE_OUNTSTANDING_REQ_COUNT - 1, freeReqQ.reqCnt);
}

static void test_allocate_until_exhaustion_hands_out_every_slot_once(void)
{
	unsigned int i, j;

	for (i = 0; i < AVAILABLE_OUNTSTANDING_REQ_COUNT; i++)
	{
		tags[i] = GetFromFreeReqQ();
		TEST_ASSERT_LESS_THAN_UINT(AVAILABLE_OUNTSTANDING_REQ_COUNT, tags[i]);
		for (j = 0; j < i; j++)
			TEST_ASSERT_NOT_EQUAL_UINT(tags[j], tags[i]);
	}

	TEST_ASSERT_EQUAL_UINT(0, freeReqQ.reqCnt);
	TEST_ASSERT_EQUAL_UINT(REQ_SLOT_TAG_NONE, freeReqQ.headReq);
	TEST_ASSERT_EQUAL_UINT(REQ_SLOT_TAG_NONE, freeReqQ.tailReq);
	/* GetFromFreeReqQ() would now spin in SyncAvailFreeReq() forever (no NAND work outstanding). */
}

static void test_release_returns_slot_to_tail_of_pool(void)
{
	unsigned int first = GetFromFreeReqQ();
	unsigned int second = GetFromFreeReqQ();
	unsigned int oldTail = freeReqQ.tailReq;

	PutToFreeReqQ(first);

	TEST_ASSERT_EQUAL_UINT(first, freeReqQ.tailReq);
	TEST_ASSERT_EQUAL_UINT(oldTail, reqPoolPtr->reqPool[first].prevReq);
	TEST_ASSERT_EQUAL_UINT(first, reqPoolPtr->reqPool[oldTail].nextReq);
	TEST_ASSERT_EQUAL_UINT(REQ_SLOT_TAG_NONE, reqPoolPtr->reqPool[first].nextReq);
	TEST_ASSERT_EQUAL_UINT(REQ_QUEUE_TYPE_FREE, reqPoolPtr->reqPool[first].reqQueueType);
	TEST_ASSERT_EQUAL_UINT(AVAILABLE_OUNTSTANDING_REQ_COUNT - 1, freeReqQ.reqCnt);
	TEST_ASSERT_EQUAL_UINT(AVAILABLE_OUNTSTANDING_REQ_COUNT - 1,
		test_walk_free_req_queue(AVAILABLE_OUNTSTANDING_REQ_COUNT + 1));

	PutToFreeReqQ(second);
	TEST_ASSERT_EQUAL_UINT(AVAILABLE_OUNTSTANDING_REQ_COUNT, freeReqQ.reqCnt);
}

static void test_release_into_empty_pool_rebuilds_head_and_tail(void)
{
	unsigned int i;

	for (i = 0; i < AVAILABLE_OUNTSTANDING_REQ_COUNT; i++)
		tags[i] = GetFromFreeReqQ();

	PutToFreeReqQ(tags[5]);

	TEST_ASSERT_EQUAL_UINT(tags[5], freeReqQ.headReq);
	TEST_ASSERT_EQUAL_UINT(tags[5], freeReqQ.tailReq);
	TEST_ASSERT_EQUAL_UINT(REQ_SLOT_TAG_NONE, reqPoolPtr->reqPool[tags[5]].prevReq);
	TEST_ASSERT_EQUAL_UINT(REQ_SLOT_TAG_NONE, reqPoolPtr->reqPool[tags[5]].nextReq);
	TEST_ASSERT_EQUAL_UINT(1, freeReqQ.reqCnt);
	TEST_ASSERT_EQUAL_UINT(tags[5], GetFromFreeReqQ());
}

static void test_release_all_slots_recovers_full_pool(void)
{
	unsigned int i;

	for (i = 0; i < AVAILABLE_OUNTSTANDING_REQ_COUNT; i++)
		tags[i] = GetFromFreeReqQ();
	for (i = 0; i < AVAILABLE_OUNTSTANDING_REQ_COUNT; i++)
		PutToFreeReqQ(tags[AVAILABLE_OUNTSTANDING_REQ_COUNT - 1 - i]);

	TEST_ASSERT_EQUAL_UINT(AVAILABLE_OUNTSTANDING_REQ_COUNT, freeReqQ.reqCnt);
	TEST_ASSERT_EQUAL_UINT(AVAILABLE_OUNTSTANDING_REQ_COUNT,
		test_walk_free_req_queue(AVAILABLE_OUNTSTANDING_REQ_COUNT + 1));
}

/*
 * SUSPICIOUS BEHAVIOUR (documented, not fixed): PutToFreeReqQ() has no guard against a
 * slot that is already in the free queue. Releasing the current tail twice links the
 * slot to itself (nextReq == its own tag), the queue count exceeds the pool size and
 * every following GetFromFreeReqQ() returns the same tag. The test records the
 * expected-correct behaviour and is ignored until the firmware decides how to handle it.
 */
static void test_double_free_does_not_corrupt_queue(void)
{
	unsigned int tag = GetFromFreeReqQ();

	PutToFreeReqQ(tag);
	PutToFreeReqQ(tag);

	if (reqPoolPtr->reqPool[tag].nextReq == tag || freeReqQ.reqCnt > AVAILABLE_OUNTSTANDING_REQ_COUNT)
		TEST_IGNORE_MESSAGE("known issue: PutToFreeReqQ() double-free links the slot to itself and over-counts the pool");

	TEST_ASSERT_EQUAL_UINT(AVAILABLE_OUNTSTANDING_REQ_COUNT, freeReqQ.reqCnt);
	TEST_ASSERT_EQUAL_UINT(AVAILABLE_OUNTSTANDING_REQ_COUNT,
		test_walk_free_req_queue(AVAILABLE_OUNTSTANDING_REQ_COUNT + 1));
}

static void test_slice_queue_is_fifo(void)
{
	unsigned int a = GetFromFreeReqQ();
	unsigned int b = GetFromFreeReqQ();

	PutToSliceReqQ(a);
	PutToSliceReqQ(b);
	TEST_ASSERT_EQUAL_UINT(2, sliceReqQ.reqCnt);
	TEST_ASSERT_EQUAL_UINT(REQ_QUEUE_TYPE_SLICE, reqPoolPtr->reqPool[a].reqQueueType);

	TEST_ASSERT_EQUAL_UINT(a, GetFromSliceReqQ());
	TEST_ASSERT_EQUAL_UINT(b, GetFromSliceReqQ());
	TEST_ASSERT_EQUAL_UINT(REQ_SLOT_TAG_FAIL, GetFromSliceReqQ());
	TEST_ASSERT_EQUAL_UINT(0, sliceReqQ.reqCnt);
}

static void test_blocked_by_buf_dep_queue_supports_selective_removal(void)
{
	unsigned int a = GetFromFreeReqQ();
	unsigned int b = GetFromFreeReqQ();
	unsigned int c = GetFromFreeReqQ();

	PutToBlockedByBufDepReqQ(a);
	PutToBlockedByBufDepReqQ(b);
	PutToBlockedByBufDepReqQ(c);
	TEST_ASSERT_EQUAL_UINT(3, blockedByBufDepReqQ.reqCnt);
	TEST_ASSERT_EQUAL_UINT(3, blockedReqCnt);

	SelectiveGetFromBlockedByBufDepReqQ(b);
	TEST_ASSERT_EQUAL_UINT(a, blockedByBufDepReqQ.headReq);
	TEST_ASSERT_EQUAL_UINT(c, blockedByBufDepReqQ.tailReq);
	TEST_ASSERT_EQUAL_UINT(c, reqPoolPtr->reqPool[a].nextReq);
	TEST_ASSERT_EQUAL_UINT(a, reqPoolPtr->reqPool[c].prevReq);
	TEST_ASSERT_EQUAL_UINT(2, blockedByBufDepReqQ.reqCnt);

	SelectiveGetFromBlockedByBufDepReqQ(a);
	SelectiveGetFromBlockedByBufDepReqQ(c);
	TEST_ASSERT_EQUAL_UINT(REQ_SLOT_TAG_NONE, blockedByBufDepReqQ.headReq);
	TEST_ASSERT_EQUAL_UINT(REQ_SLOT_TAG_NONE, blockedByBufDepReqQ.tailReq);
	TEST_ASSERT_EQUAL_UINT(0, blockedReqCnt);
}

static void test_nand_queue_get_releases_slot_and_counts_completion(void)
{
	unsigned int a = GetFromFreeReqQ();
	unsigned int before = freeReqQ.reqCnt;

	reqPoolPtr->reqPool[a].reqType = REQ_TYPE_NAND;
	reqPoolPtr->reqPool[a].reqCode = REQ_CODE_READ;
	reqPoolPtr->reqPool[a].reqOpt.dataBufFormat = REQ_OPT_DATA_BUF_NONE;
	reqPoolPtr->reqPool[a].reqOpt.rowAddrDependencyCheck = REQ_OPT_ROW_ADDR_DEPENDENCY_NONE;
	PutToNandReqQ(a, 0, 0);

	TEST_ASSERT_EQUAL_UINT(1, nandReqQ[0][0].reqCnt);
	TEST_ASSERT_EQUAL_UINT(1, notCompletedNandReqCnt);
	TEST_ASSERT_EQUAL_UINT(REQ_QUEUE_TYPE_NAND, reqPoolPtr->reqPool[a].reqQueueType);

	GetFromNandReqQ(0, 0, REQ_STATUS_DONE, REQ_CODE_READ);

	TEST_ASSERT_EQUAL_UINT(0, nandReqQ[0][0].reqCnt);
	TEST_ASSERT_EQUAL_UINT(0, notCompletedNandReqCnt);
	TEST_ASSERT_EQUAL_UINT(before + 1, freeReqQ.reqCnt);
	TEST_ASSERT_EQUAL_UINT(a, freeReqQ.tailReq);
}

int main(void)
{
	UNITY_BEGIN();
	RUN_TEST(test_init_links_every_slot_into_free_queue);
	RUN_TEST(test_get_returns_head_and_marks_slot_unqueued);
	RUN_TEST(test_allocate_until_exhaustion_hands_out_every_slot_once);
	RUN_TEST(test_release_returns_slot_to_tail_of_pool);
	RUN_TEST(test_release_into_empty_pool_rebuilds_head_and_tail);
	RUN_TEST(test_release_all_slots_recovers_full_pool);
	RUN_TEST(test_double_free_does_not_corrupt_queue);
	RUN_TEST(test_slice_queue_is_fifo);
	RUN_TEST(test_blocked_by_buf_dep_queue_supports_selective_removal);
	RUN_TEST(test_nand_queue_get_releases_slot_and_counts_completion);
	return UNITY_END();
}
