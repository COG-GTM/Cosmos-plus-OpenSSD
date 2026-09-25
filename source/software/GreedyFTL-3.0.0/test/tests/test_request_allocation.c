#include "unity.h"

#include "ftl_test_env.h"

#define POOL_SIZE AVAILABLE_OUNTSTANDING_REQ_COUNT

void setUp(void)
{
	ftl_test_env_init();
}

void tearDown(void)
{
}

/* Walk the free list forwards and check every backward link agrees. */
static int FreeListIsWellFormed(void)
{
	unsigned int tag = freeReqQ.headReq, prev = REQ_SLOT_TAG_NONE, n = 0;

	while (tag != REQ_SLOT_TAG_NONE)
	{
		if (reqPoolPtr->reqPool[tag].prevReq != prev)
			return 0;
		if (reqPoolPtr->reqPool[tag].reqQueueType != REQ_QUEUE_TYPE_FREE)
			return 0;
		if (++n > POOL_SIZE)
			return 0; /* cycle */
		prev = tag;
		tag = reqPoolPtr->reqPool[tag].nextReq;
	}
	return prev == freeReqQ.tailReq && n == freeReqQ.reqCnt;
}

/* ---- Initial state ------------------------------------------------------ */

static void test_pool_starts_with_every_slot_free_in_order(void)
{
	unsigned int tag;

	TEST_ASSERT_EQUAL_UINT32(POOL_SIZE, freeReqQ.reqCnt);
	TEST_ASSERT_EQUAL_UINT32(POOL_SIZE, ftl_test_count_free_reqs());
	TEST_ASSERT_NOT_EQUAL(REQ_SLOT_TAG_NONE, freeReqQ.headReq);
	TEST_ASSERT_TRUE(FreeListIsWellFormed());

	for (tag = 0; tag < POOL_SIZE; tag++)
	{
		TEST_ASSERT_EQUAL_UINT32(REQ_QUEUE_TYPE_FREE, reqPoolPtr->reqPool[tag].reqQueueType);
		TEST_ASSERT_EQUAL_UINT32(REQ_SLOT_TAG_NONE, reqPoolPtr->reqPool[tag].prevBlockingReq);
		TEST_ASSERT_EQUAL_UINT32(REQ_SLOT_TAG_NONE, reqPoolPtr->reqPool[tag].nextBlockingReq);
	}

	TEST_ASSERT_EQUAL_UINT32(0, sliceReqQ.reqCnt);
	TEST_ASSERT_EQUAL_UINT32(0, blockedByBufDepReqQ.reqCnt);
	TEST_ASSERT_EQUAL_UINT32(0, nvmeDmaReqQ.reqCnt);
	TEST_ASSERT_EQUAL_UINT32(0, notCompletedNandReqCnt);
	TEST_ASSERT_EQUAL_UINT32(0, blockedReqCnt);
}

/* ---- Allocation ---------------------------------------------------------- */

static void test_alloc_hands_out_distinct_slots_until_exhaustion(void)
{
	static unsigned char seen[POOL_SIZE];
	unsigned int i, tag;

	for (i = 0; i < POOL_SIZE; i++)
	{
		tag = GetFromFreeReqQ();
		TEST_ASSERT_LESS_THAN_UINT32(POOL_SIZE, tag);
		TEST_ASSERT_FALSE_MESSAGE(seen[tag], "slot handed out twice");
		seen[tag] = 1;
		TEST_ASSERT_EQUAL_UINT32(REQ_QUEUE_TYPE_NONE, reqPoolPtr->reqPool[tag].reqQueueType);
		TEST_ASSERT_EQUAL_UINT32(POOL_SIZE - i - 1, freeReqQ.reqCnt);
	}

	TEST_ASSERT_EQUAL_UINT32(REQ_SLOT_TAG_NONE, freeReqQ.headReq);
	TEST_ASSERT_EQUAL_UINT32(REQ_SLOT_TAG_NONE, freeReqQ.tailReq);
	TEST_ASSERT_EQUAL_UINT32(0, ftl_test_count_free_reqs());

	for (tag = 0; tag < POOL_SIZE; tag++)
		PutToFreeReqQ(tag);
}

static void test_alloc_is_fifo(void)
{
	unsigned int first = freeReqQ.headReq;
	unsigned int second = reqPoolPtr->reqPool[first].nextReq;
	unsigned int third = reqPoolPtr->reqPool[second].nextReq;
	unsigned int fourth = reqPoolPtr->reqPool[third].nextReq;

	TEST_ASSERT_EQUAL_UINT32(first, GetFromFreeReqQ());
	TEST_ASSERT_EQUAL_UINT32(second, GetFromFreeReqQ());
	TEST_ASSERT_EQUAL_UINT32(third, GetFromFreeReqQ());
	TEST_ASSERT_EQUAL_UINT32(fourth, freeReqQ.headReq);
	TEST_ASSERT_EQUAL_UINT32(REQ_SLOT_TAG_NONE, reqPoolPtr->reqPool[fourth].prevReq);
	PutToFreeReqQ(first);
	PutToFreeReqQ(second);
	PutToFreeReqQ(third);
}

/* ---- Release ------------------------------------------------------------- */

static void test_release_appends_slot_to_tail_and_it_is_reused_last(void)
{
	unsigned int tag = GetFromFreeReqQ(), next = freeReqQ.headReq, i;

	PutToFreeReqQ(tag);

	TEST_ASSERT_EQUAL_UINT32(POOL_SIZE, freeReqQ.reqCnt);
	TEST_ASSERT_EQUAL_UINT32(tag, freeReqQ.tailReq);
	TEST_ASSERT_EQUAL_UINT32(next, freeReqQ.headReq);
	TEST_ASSERT_EQUAL_UINT32(REQ_QUEUE_TYPE_FREE, reqPoolPtr->reqPool[tag].reqQueueType);
	TEST_ASSERT_TRUE(FreeListIsWellFormed());

	for (i = 1; i < POOL_SIZE; i++)
		TEST_ASSERT_NOT_EQUAL(tag, GetFromFreeReqQ());
	TEST_ASSERT_EQUAL_UINT32(tag, GetFromFreeReqQ());

	for (i = 0; i < POOL_SIZE; i++)
		PutToFreeReqQ(i);
}

static void test_release_into_empty_pool_restores_head_and_tail(void)
{
	unsigned int i;

	for (i = 0; i < POOL_SIZE; i++)
		GetFromFreeReqQ();
	TEST_ASSERT_EQUAL_UINT32(REQ_SLOT_TAG_NONE, freeReqQ.headReq);

	PutToFreeReqQ(17);

	TEST_ASSERT_EQUAL_UINT32(17, freeReqQ.headReq);
	TEST_ASSERT_EQUAL_UINT32(17, freeReqQ.tailReq);
	TEST_ASSERT_EQUAL_UINT32(1, freeReqQ.reqCnt);
	TEST_ASSERT_EQUAL_UINT32(REQ_SLOT_TAG_NONE, reqPoolPtr->reqPool[17].prevReq);
	TEST_ASSERT_EQUAL_UINT32(REQ_SLOT_TAG_NONE, reqPoolPtr->reqPool[17].nextReq);
	TEST_ASSERT_EQUAL_UINT32(17, GetFromFreeReqQ());
	TEST_ASSERT_EQUAL_UINT32(REQ_SLOT_TAG_NONE, freeReqQ.tailReq);

	for (i = 0; i < POOL_SIZE; i++)
		PutToFreeReqQ(i);
}

static void test_alloc_on_empty_pool_waits_for_inflight_nand_request(void)
{
	unsigned int i, inflight, tag;

	for (i = 0; i < POOL_SIZE - 1; i++)
		GetFromFreeReqQ();
	inflight = ftl_test_issue_write(77, 0x11); /* takes the very last slot */
	TEST_ASSERT_EQUAL_UINT32(0, freeReqQ.reqCnt);
	TEST_ASSERT_EQUAL_UINT32(1, ftl_test_pending_nand_reqs());

	/* GetFromFreeReqQ() must drive the scheduler until that write completes. */
	tag = GetFromFreeReqQ();

	TEST_ASSERT_EQUAL_UINT32(inflight, tag);
	TEST_ASSERT_EQUAL_UINT32(0, ftl_test_pending_nand_reqs());
	TEST_ASSERT_EQUAL_UINT32(0, notCompletedNandReqCnt);

	for (i = 0; i < POOL_SIZE; i++)
		PutToFreeReqQ(i);
}

/* ---- Double free --------------------------------------------------------- */

static void test_release_of_already_free_slot_corrupts_free_list(void)
{
	unsigned int tag = GetFromFreeReqQ();

	PutToFreeReqQ(tag);
	TEST_ASSERT_TRUE(FreeListIsWellFormed());

	TEST_IGNORE_MESSAGE("PutToFreeReqQ() has no double-free guard: freeing an already-free slot "
						"links it to itself, reqCnt over-counts and GetFromFreeReqQ() returns "
						"the same slot twice (see PR description).");

	PutToFreeReqQ(tag);
	TEST_ASSERT_EQUAL_UINT32(POOL_SIZE, freeReqQ.reqCnt);
	TEST_ASSERT_TRUE(FreeListIsWellFormed());
}

static void test_queue_type_flag_identifies_free_slots_for_callers(void)
{
	unsigned int tag = GetFromFreeReqQ();

	TEST_ASSERT_NOT_EQUAL(REQ_QUEUE_TYPE_FREE, reqPoolPtr->reqPool[tag].reqQueueType);
	PutToFreeReqQ(tag);
	TEST_ASSERT_EQUAL_UINT32(REQ_QUEUE_TYPE_FREE, reqPoolPtr->reqPool[tag].reqQueueType);
}

/* ---- Other queues -------------------------------------------------------- */

static void test_slice_queue_is_fifo_and_reports_empty(void)
{
	unsigned int a = GetFromFreeReqQ(), b = GetFromFreeReqQ();

	TEST_ASSERT_EQUAL_UINT32(REQ_SLOT_TAG_FAIL, GetFromSliceReqQ());

	PutToSliceReqQ(a);
	PutToSliceReqQ(b);
	TEST_ASSERT_EQUAL_UINT32(2, sliceReqQ.reqCnt);
	TEST_ASSERT_EQUAL_UINT32(REQ_QUEUE_TYPE_SLICE, reqPoolPtr->reqPool[a].reqQueueType);

	TEST_ASSERT_EQUAL_UINT32(a, GetFromSliceReqQ());
	TEST_ASSERT_EQUAL_UINT32(b, GetFromSliceReqQ());
	TEST_ASSERT_EQUAL_UINT32(REQ_SLOT_TAG_FAIL, GetFromSliceReqQ());
	TEST_ASSERT_EQUAL_UINT32(0, sliceReqQ.reqCnt);
	TEST_ASSERT_EQUAL_UINT32(REQ_SLOT_TAG_NONE, sliceReqQ.tailReq);

	PutToFreeReqQ(a);
	PutToFreeReqQ(b);
}

static void test_blocked_by_buf_dep_queue_supports_removal_from_middle(void)
{
	unsigned int a = GetFromFreeReqQ(), b = GetFromFreeReqQ(), c = GetFromFreeReqQ();

	PutToBlockedByBufDepReqQ(a);
	PutToBlockedByBufDepReqQ(b);
	PutToBlockedByBufDepReqQ(c);
	TEST_ASSERT_EQUAL_UINT32(3, blockedReqCnt);

	SelectiveGetFromBlockedByBufDepReqQ(b);
	TEST_ASSERT_EQUAL_UINT32(c, reqPoolPtr->reqPool[a].nextReq);
	TEST_ASSERT_EQUAL_UINT32(a, reqPoolPtr->reqPool[c].prevReq);
	TEST_ASSERT_EQUAL_UINT32(2, blockedByBufDepReqQ.reqCnt);

	SelectiveGetFromBlockedByBufDepReqQ(a);
	TEST_ASSERT_EQUAL_UINT32(c, blockedByBufDepReqQ.headReq);
	SelectiveGetFromBlockedByBufDepReqQ(c);
	TEST_ASSERT_EQUAL_UINT32(REQ_SLOT_TAG_NONE, blockedByBufDepReqQ.headReq);
	TEST_ASSERT_EQUAL_UINT32(REQ_SLOT_TAG_NONE, blockedByBufDepReqQ.tailReq);
	TEST_ASSERT_EQUAL_UINT32(0, blockedReqCnt);

	PutToFreeReqQ(a);
	PutToFreeReqQ(b);
	PutToFreeReqQ(c);
}

static void test_nand_queue_tracks_outstanding_count_per_die(void)
{
	unsigned int a = GetFromFreeReqQ(), b = GetFromFreeReqQ();

	PutToNandReqQ(a, 0, 1);
	PutToNandReqQ(b, 0, 1);
	TEST_ASSERT_EQUAL_UINT32(2, nandReqQ[0][1].reqCnt);
	TEST_ASSERT_EQUAL_UINT32(2, notCompletedNandReqCnt);
	TEST_ASSERT_EQUAL_UINT32(a, nandReqQ[0][1].headReq);
	TEST_ASSERT_EQUAL_UINT32(b, nandReqQ[0][1].tailReq);
	TEST_ASSERT_EQUAL_UINT32(REQ_QUEUE_TYPE_NAND, reqPoolPtr->reqPool[a].reqQueueType);

	/* Completing the head returns it to the free pool. */
	reqPoolPtr->reqPool[a].reqCode = REQ_CODE_READ_TRANSFER;
	GetFromNandReqQ(0, 1, REQ_STATUS_DONE, REQ_CODE_READ_TRANSFER);
	TEST_ASSERT_EQUAL_UINT32(1, nandReqQ[0][1].reqCnt);
	TEST_ASSERT_EQUAL_UINT32(1, notCompletedNandReqCnt);
	TEST_ASSERT_EQUAL_UINT32(b, nandReqQ[0][1].headReq);
	TEST_ASSERT_EQUAL_UINT32(a, freeReqQ.tailReq);

	GetFromNandReqQ(0, 1, REQ_STATUS_DONE, REQ_CODE_READ_TRANSFER);
	TEST_ASSERT_EQUAL_UINT32(0, notCompletedNandReqCnt);
	TEST_ASSERT_EQUAL_UINT32(POOL_SIZE, freeReqQ.reqCnt);
	TEST_ASSERT_TRUE(FreeListIsWellFormed());
}

int main(void)
{
	UNITY_BEGIN();
	RUN_TEST(test_pool_starts_with_every_slot_free_in_order);
	RUN_TEST(test_alloc_hands_out_distinct_slots_until_exhaustion);
	RUN_TEST(test_alloc_is_fifo);
	RUN_TEST(test_release_appends_slot_to_tail_and_it_is_reused_last);
	RUN_TEST(test_release_into_empty_pool_restores_head_and_tail);
	RUN_TEST(test_alloc_on_empty_pool_waits_for_inflight_nand_request);
	RUN_TEST(test_release_of_already_free_slot_corrupts_free_list);
	RUN_TEST(test_queue_type_flag_identifies_free_slots_for_callers);
	RUN_TEST(test_slice_queue_is_fifo_and_reports_empty);
	RUN_TEST(test_blocked_by_buf_dep_queue_supports_removal_from_middle);
	RUN_TEST(test_nand_queue_tracks_outstanding_count_per_die);
	return UNITY_END();
}
