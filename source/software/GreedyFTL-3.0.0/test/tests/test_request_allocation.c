#include "test_support.h"

void setUp(void) { TestFtlReset(); }
void tearDown(void) {}

#define POOL_SIZE AVAILABLE_OUNTSTANDING_REQ_COUNT

static unsigned int WalkQueue(const FREE_REQUEST_QUEUE *q)
{
	unsigned int tag = q->headReq, n = 0;
	while (tag != REQ_SLOT_TAG_NONE && n <= POOL_SIZE)
	{
		n++;
		tag = reqPoolPtr->reqPool[tag].nextReq;
	}
	return n;
}

static unsigned int ReqsInQueueType(unsigned int queueType)
{
	unsigned int tag, n = 0;
	for (tag = 0; tag < POOL_SIZE; tag++)
		if (reqPoolPtr->reqPool[tag].reqQueueType == queueType)
			n++;
	return n;
}

/* Parks a request in the NVMe DMA queue in a state the fake DMA engine reports as complete. */
static unsigned int ParkCompletedTxDma(void)
{
	unsigned int tag = GetFromFreeReqQ();
	reqPoolPtr->reqPool[tag].reqType = REQ_TYPE_NVME_DMA;
	reqPoolPtr->reqPool[tag].reqCode = REQ_CODE_TxDMA;
	reqPoolPtr->reqPool[tag].reqOpt.dataBufFormat = REQ_OPT_DATA_BUF_ENTRY;
	reqPoolPtr->reqPool[tag].dataBufInfo.entry = 0;
	reqPoolPtr->reqPool[tag].nvmeDmaInfo.reqTail = g_hostDmaStatus.fifoTail.autoDmaTx;
	reqPoolPtr->reqPool[tag].nvmeDmaInfo.overFlowCnt = g_hostDmaAssistStatus.autoDmaTxOverFlowCnt;
	PutToNvmeDmaReqQ(tag);
	return tag;
}

/* ---------------------------------------------------------------- init */

void test_init_puts_every_slot_in_free_queue_in_order(void)
{
	unsigned int tag;

	TEST_ASSERT_EQUAL_UINT(POOL_SIZE, freeReqQ.reqCnt);
	TEST_ASSERT_EQUAL_UINT(0, freeReqQ.headReq);
	TEST_ASSERT_EQUAL_UINT(POOL_SIZE - 1, freeReqQ.tailReq);
	TEST_ASSERT_EQUAL_UINT(POOL_SIZE, WalkQueue(&freeReqQ));
	for (tag = 0; tag < POOL_SIZE; tag++)
		TEST_ASSERT_EQUAL_UINT(REQ_QUEUE_TYPE_FREE, reqPoolPtr->reqPool[tag].reqQueueType);
	TEST_ASSERT_EQUAL_UINT(0, sliceReqQ.reqCnt);
	TEST_ASSERT_EQUAL_UINT(0, blockedByBufDepReqQ.reqCnt);
	TEST_ASSERT_EQUAL_UINT(0, nvmeDmaReqQ.reqCnt);
	TEST_ASSERT_EQUAL_UINT(0, blockedReqCnt);
	TEST_ASSERT_EQUAL_UINT(0, notCompletedNandReqCnt);
}

/* ---------------------------------------------------------------- alloc / free */

void test_alloc_hands_out_head_and_marks_slot_not_free(void)
{
	unsigned int tag = GetFromFreeReqQ();
	TEST_ASSERT_EQUAL_UINT(0, tag);
	TEST_ASSERT_EQUAL_UINT(1, freeReqQ.headReq);
	TEST_ASSERT_EQUAL_UINT(REQ_QUEUE_TYPE_NONE, reqPoolPtr->reqPool[tag].reqQueueType);
	TEST_ASSERT_EQUAL_UINT(POOL_SIZE - 1, freeReqQ.reqCnt);
	TEST_ASSERT_EQUAL_UINT(POOL_SIZE - 1, WalkQueue(&freeReqQ));
}

void test_alloc_until_exhaustion_yields_each_slot_exactly_once(void)
{
	unsigned char seen[POOL_SIZE] = {0};
	unsigned int i, tag;

	for (i = 0; i < POOL_SIZE; i++)
	{
		tag = GetFromFreeReqQ();
		TEST_ASSERT_LESS_THAN_UINT(POOL_SIZE, tag);
		TEST_ASSERT_EQUAL_UINT_MESSAGE(0, seen[tag], "slot handed out twice");
		seen[tag] = 1;
	}
	TEST_ASSERT_EQUAL_UINT(0, freeReqQ.reqCnt);
	TEST_ASSERT_EQUAL_UINT(REQ_SLOT_TAG_NONE, freeReqQ.headReq);
	TEST_ASSERT_EQUAL_UINT(REQ_SLOT_TAG_NONE, freeReqQ.tailReq);
	TEST_ASSERT_EQUAL_UINT(0, ReqsInQueueType(REQ_QUEUE_TYPE_FREE));
}

void test_release_returns_slot_to_tail_of_free_queue(void)
{
	unsigned int a = GetFromFreeReqQ(), b = GetFromFreeReqQ();

	PutToFreeReqQ(a);
	TEST_ASSERT_EQUAL_UINT(a, freeReqQ.tailReq);
	TEST_ASSERT_EQUAL_UINT(REQ_QUEUE_TYPE_FREE, reqPoolPtr->reqPool[a].reqQueueType);
	TEST_ASSERT_EQUAL_UINT(POOL_SIZE - 1, freeReqQ.reqCnt);
	TEST_ASSERT_EQUAL_UINT(POOL_SIZE - 1, WalkQueue(&freeReqQ));

	PutToFreeReqQ(b);
	TEST_ASSERT_EQUAL_UINT(b, freeReqQ.tailReq);
	TEST_ASSERT_EQUAL_UINT(a, reqPoolPtr->reqPool[b].prevReq);
	TEST_ASSERT_EQUAL_UINT(POOL_SIZE, freeReqQ.reqCnt);
}

void test_release_into_empty_pool_restores_head_and_tail(void)
{
	unsigned int i, last = 0;

	for (i = 0; i < POOL_SIZE; i++)
		last = GetFromFreeReqQ();

	PutToFreeReqQ(last);
	TEST_ASSERT_EQUAL_UINT(last, freeReqQ.headReq);
	TEST_ASSERT_EQUAL_UINT(last, freeReqQ.tailReq);
	TEST_ASSERT_EQUAL_UINT(REQ_SLOT_TAG_NONE, reqPoolPtr->reqPool[last].prevReq);
	TEST_ASSERT_EQUAL_UINT(REQ_SLOT_TAG_NONE, reqPoolPtr->reqPool[last].nextReq);
	TEST_ASSERT_EQUAL_UINT(1, freeReqQ.reqCnt);
	TEST_ASSERT_EQUAL_UINT(last, GetFromFreeReqQ());
}

void test_alloc_after_full_release_cycle_reuses_all_slots(void)
{
	unsigned int tags[POOL_SIZE];
	unsigned int i;

	for (i = 0; i < POOL_SIZE; i++)
		tags[i] = GetFromFreeReqQ();
	for (i = 0; i < POOL_SIZE; i++)
		PutToFreeReqQ(tags[POOL_SIZE - 1 - i]);  /* release in reverse */

	TEST_ASSERT_EQUAL_UINT(POOL_SIZE, freeReqQ.reqCnt);
	TEST_ASSERT_EQUAL_UINT(POOL_SIZE, WalkQueue(&freeReqQ));
	TEST_ASSERT_EQUAL_UINT(tags[POOL_SIZE - 1], GetFromFreeReqQ());  /* FIFO: first released first out */
}

void test_alloc_on_exhausted_pool_waits_for_inflight_dma_to_complete(void)
{
	unsigned int parked = ParkCompletedTxDma();
	unsigned int i, got;

	for (i = 0; i < POOL_SIZE - 1; i++)
		GetFromFreeReqQ();
	TEST_ASSERT_EQUAL_UINT(0, freeReqQ.reqCnt);
	TEST_ASSERT_EQUAL_UINT(1, nvmeDmaReqQ.reqCnt);

	/* The next allocation has to reap the finished DMA request to make progress. */
	got = GetFromFreeReqQ();
	TEST_ASSERT_EQUAL_UINT(parked, got);
	TEST_ASSERT_EQUAL_UINT(0, nvmeDmaReqQ.reqCnt);
	TEST_ASSERT_EQUAL_UINT(0, freeReqQ.reqCnt);
}

/* ---------------------------------------------------------------- double free */

/* Desired invariant: releasing a slot that is already free must not change the pool.
 * The firmware's PutToFreeReqQ() has no guard, so this currently fails; it is kept as a
 * skipped test to document the hazard without changing firmware behavior. */
void test_double_free_is_rejected(void)
{
	unsigned int tag = GetFromFreeReqQ();

	TEST_IGNORE_MESSAGE("KNOWN FIRMWARE HAZARD: PutToFreeReqQ() accepts an already-free slot "
						"(reqCnt overcounts, free list gets a self-loop). See test/README.md.");

	PutToFreeReqQ(tag);
	PutToFreeReqQ(tag);
	TEST_ASSERT_EQUAL_UINT(POOL_SIZE, freeReqQ.reqCnt);
	TEST_ASSERT_EQUAL_UINT(POOL_SIZE, WalkQueue(&freeReqQ));
}

/* Pins the observable symptom so a future fix (or regression) shows up in the suite. */
void test_double_free_currently_corrupts_free_queue_characterization(void)
{
	unsigned int tag = GetFromFreeReqQ();

	PutToFreeReqQ(tag);
	PutToFreeReqQ(tag);

	TEST_ASSERT_EQUAL_UINT(POOL_SIZE + 1, freeReqQ.reqCnt);              /* overcounted */
	TEST_ASSERT_EQUAL_UINT(tag, reqPoolPtr->reqPool[tag].prevReq);       /* self-loop at the tail */
	TEST_ASSERT_EQUAL_UINT(tag, freeReqQ.tailReq);
	TEST_ASSERT_GREATER_THAN_UINT(POOL_SIZE, WalkQueue(&freeReqQ));      /* list no longer terminates */
}

/* ---------------------------------------------------------------- other queues */

void test_slice_queue_is_fifo(void)
{
	unsigned int a = GetFromFreeReqQ(), b = GetFromFreeReqQ();

	PutToSliceReqQ(a);
	PutToSliceReqQ(b);
	TEST_ASSERT_EQUAL_UINT(2, sliceReqQ.reqCnt);
	TEST_ASSERT_EQUAL_UINT(REQ_QUEUE_TYPE_SLICE, reqPoolPtr->reqPool[a].reqQueueType);

	TEST_ASSERT_EQUAL_UINT(a, GetFromSliceReqQ());
	TEST_ASSERT_EQUAL_UINT(b, GetFromSliceReqQ());
	TEST_ASSERT_EQUAL_UINT(REQ_SLOT_TAG_NONE, GetFromSliceReqQ());
	TEST_ASSERT_EQUAL_UINT(0, sliceReqQ.reqCnt);
}

void test_blocked_by_buf_dep_queue_supports_removal_from_any_position(void)
{
	unsigned int a = GetFromFreeReqQ(), b = GetFromFreeReqQ(), c = GetFromFreeReqQ();

	PutToBlockedByBufDepReqQ(a);
	PutToBlockedByBufDepReqQ(b);
	PutToBlockedByBufDepReqQ(c);
	TEST_ASSERT_EQUAL_UINT(3, blockedReqCnt);

	SelectiveGetFromBlockedByBufDepReqQ(b);
	TEST_ASSERT_EQUAL_UINT(c, reqPoolPtr->reqPool[a].nextReq);
	TEST_ASSERT_EQUAL_UINT(a, reqPoolPtr->reqPool[c].prevReq);
	SelectiveGetFromBlockedByBufDepReqQ(c);
	TEST_ASSERT_EQUAL_UINT(a, blockedByBufDepReqQ.tailReq);
	SelectiveGetFromBlockedByBufDepReqQ(a);
	TEST_ASSERT_EQUAL_UINT(REQ_SLOT_TAG_NONE, blockedByBufDepReqQ.headReq);
	TEST_ASSERT_EQUAL_UINT(0, blockedByBufDepReqQ.reqCnt);
	TEST_ASSERT_EQUAL_UINT(0, blockedReqCnt);
}

void test_blocked_by_row_addr_dep_queue_is_per_die(void)
{
	unsigned int a = GetFromFreeReqQ(), b = GetFromFreeReqQ();

	PutToBlockedByRowAddrDepReqQ(a, 0, 0);
	PutToBlockedByRowAddrDepReqQ(b, 1, 0);
	TEST_ASSERT_EQUAL_UINT(1, blockedByRowAddrDepReqQ[0][0].reqCnt);
	TEST_ASSERT_EQUAL_UINT(1, blockedByRowAddrDepReqQ[1][0].reqCnt);
	TEST_ASSERT_EQUAL_UINT(2, blockedReqCnt);

	SelectiveGetFromBlockedByRowAddrDepReqQ(a, 0, 0);
	TEST_ASSERT_EQUAL_UINT(0, blockedByRowAddrDepReqQ[0][0].reqCnt);
	TEST_ASSERT_EQUAL_UINT(REQ_SLOT_TAG_NONE, blockedByRowAddrDepReqQ[0][0].headReq);
	TEST_ASSERT_EQUAL_UINT(1, blockedReqCnt);
}

void test_nand_queue_tracks_outstanding_count_and_completion_frees_slot(void)
{
	unsigned int tag = GetFromFreeReqQ();
	unsigned int freeBefore = freeReqQ.reqCnt;

	reqPoolPtr->reqPool[tag].reqType = REQ_TYPE_NAND;
	reqPoolPtr->reqPool[tag].reqCode = REQ_CODE_ERASE;
	reqPoolPtr->reqPool[tag].reqOpt.dataBufFormat = REQ_OPT_DATA_BUF_NONE;
	PutToNandReqQ(tag, 1, 2);
	TEST_ASSERT_EQUAL_UINT(1, nandReqQ[1][2].reqCnt);
	TEST_ASSERT_EQUAL_UINT(1, notCompletedNandReqCnt);
	TEST_ASSERT_EQUAL_UINT(REQ_QUEUE_TYPE_NAND, reqPoolPtr->reqPool[tag].reqQueueType);

	GetFromNandReqQ(1, 2, REQ_STATUS_DONE, REQ_CODE_ERASE);
	TEST_ASSERT_EQUAL_UINT(0, nandReqQ[1][2].reqCnt);
	TEST_ASSERT_EQUAL_UINT(0, notCompletedNandReqCnt);
	TEST_ASSERT_EQUAL_UINT(freeBefore + 1, freeReqQ.reqCnt);
	TEST_ASSERT_EQUAL_UINT(tag, freeReqQ.tailReq);
}

int main(void)
{
	UNITY_BEGIN();
	RUN_TEST(test_init_puts_every_slot_in_free_queue_in_order);
	RUN_TEST(test_alloc_hands_out_head_and_marks_slot_not_free);
	RUN_TEST(test_alloc_until_exhaustion_yields_each_slot_exactly_once);
	RUN_TEST(test_release_returns_slot_to_tail_of_free_queue);
	RUN_TEST(test_release_into_empty_pool_restores_head_and_tail);
	RUN_TEST(test_alloc_after_full_release_cycle_reuses_all_slots);
	RUN_TEST(test_alloc_on_exhausted_pool_waits_for_inflight_dma_to_complete);
	RUN_TEST(test_double_free_is_rejected);
	RUN_TEST(test_double_free_currently_corrupts_free_queue_characterization);
	RUN_TEST(test_slice_queue_is_fifo);
	RUN_TEST(test_blocked_by_buf_dep_queue_supports_removal_from_any_position);
	RUN_TEST(test_blocked_by_row_addr_dep_queue_is_per_die);
	RUN_TEST(test_nand_queue_tracks_outstanding_count_and_completion_frees_slot);
	return UNITY_END();
}
