/*
 * Queue-level unit tests for request_allocation.c.
 *
 * SyncAvailFreeReq() (request_schedule.c) and ReleaseBlockedByBufDepReq()
 * (request_transform.c) are wrapped (see test_request_allocation_queues.wrap)
 * so the allocator can be exercised in isolation: pool exhaustion is
 * observable instead of spinning on the NAND scheduler, and the completion
 * paths of the NVMe DMA / NAND queues can be checked without a data buffer.
 */
#include "unity.h"

#include "fw_test.h"
#include "memory_map.h"

#define POOL_SIZE AVAILABLE_OUNTSTANDING_REQ_COUNT
#define NONE      REQ_SLOT_TAG_NONE

static unsigned int syncAvailFreeReqCalls;
static unsigned int syncAvailFreeReqRefillTag;
static unsigned int releaseBlockedCalls;
static unsigned int releaseBlockedLastTag;

void __wrap_SyncAvailFreeReq(void)
{
	syncAvailFreeReqCalls++;
	if (syncAvailFreeReqRefillTag != NONE)
		PutToFreeReqQ(syncAvailFreeReqRefillTag);
}

void __wrap_ReleaseBlockedByBufDepReq(unsigned int reqSlotTag)
{
	releaseBlockedCalls++;
	releaseBlockedLastTag = reqSlotTag;
}

void setUp(void)
{
	fw_test_reset();
	InitReqPool();
	syncAvailFreeReqCalls = 0;
	syncAvailFreeReqRefillTag = NONE;
	releaseBlockedCalls = 0;
	releaseBlockedLastTag = NONE;
}

void tearDown(void) {}

static SSD_REQ_FORMAT *req(unsigned int tag)
{
	return &reqPoolPtr->reqPool[tag];
}

/* Walk head->tail via nextReq, checking prevReq back-links, and return length. */
static unsigned int walk_list(unsigned int head, unsigned int tail)
{
	unsigned int count = 0;
	unsigned int prev = NONE;
	unsigned int cur = head;

	while (cur != NONE) {
		TEST_ASSERT_EQUAL_UINT(prev, req(cur)->prevReq);
		prev = cur;
		cur = req(cur)->nextReq;
		count++;
		TEST_ASSERT_TRUE_MESSAGE(count <= POOL_SIZE, "list cycle");
	}
	TEST_ASSERT_EQUAL_UINT(tail, prev);
	return count;
}

static unsigned int count_slots_with_queue_type(unsigned int type)
{
	unsigned int i, n = 0;

	for (i = 0; i < POOL_SIZE; i++)
		if (req(i)->reqQueueType == type)
			n++;
	return n;
}

static void assert_free_list_consistent(void)
{
	unsigned int n = walk_list(freeReqQ.headReq, freeReqQ.tailReq);

	TEST_ASSERT_EQUAL_UINT(freeReqQ.reqCnt, n);
	TEST_ASSERT_EQUAL_UINT(n, count_slots_with_queue_type(REQ_QUEUE_TYPE_FREE));
	if (n == 0) {
		TEST_ASSERT_EQUAL_UINT(NONE, freeReqQ.headReq);
		TEST_ASSERT_EQUAL_UINT(NONE, freeReqQ.tailReq);
	}
}

/* ---------------------------------------------------------------- InitReqPool */

static void test_init_links_pool_as_single_free_chain(void)
{
	unsigned int i;

	for (i = 0; i < POOL_SIZE; i++) {
		TEST_ASSERT_EQUAL_UINT(REQ_QUEUE_TYPE_FREE, req(i)->reqQueueType);
		TEST_ASSERT_EQUAL_UINT(NONE, req(i)->prevBlockingReq);
		TEST_ASSERT_EQUAL_UINT(NONE, req(i)->nextBlockingReq);
		TEST_ASSERT_EQUAL_UINT(i == 0 ? NONE : i - 1, req(i)->prevReq);
		TEST_ASSERT_EQUAL_UINT(i == POOL_SIZE - 1 ? NONE : i + 1, req(i)->nextReq);
	}
	TEST_ASSERT_EQUAL_UINT(POOL_SIZE, walk_list(freeReqQ.headReq, freeReqQ.tailReq));
}

static void test_init_empties_every_other_queue_and_counters(void)
{
	unsigned int ch, way;

	TEST_ASSERT_EQUAL_UINT(NONE, sliceReqQ.headReq);
	TEST_ASSERT_EQUAL_UINT(NONE, sliceReqQ.tailReq);
	TEST_ASSERT_EQUAL_UINT(0, sliceReqQ.reqCnt);
	TEST_ASSERT_EQUAL_UINT(NONE, blockedByBufDepReqQ.headReq);
	TEST_ASSERT_EQUAL_UINT(NONE, blockedByBufDepReqQ.tailReq);
	TEST_ASSERT_EQUAL_UINT(0, blockedByBufDepReqQ.reqCnt);
	TEST_ASSERT_EQUAL_UINT(NONE, nvmeDmaReqQ.headReq);
	TEST_ASSERT_EQUAL_UINT(NONE, nvmeDmaReqQ.tailReq);
	TEST_ASSERT_EQUAL_UINT(0, nvmeDmaReqQ.reqCnt);
	for (ch = 0; ch < USER_CHANNELS; ch++)
		for (way = 0; way < USER_WAYS; way++) {
			TEST_ASSERT_EQUAL_UINT(NONE, blockedByRowAddrDepReqQ[ch][way].headReq);
			TEST_ASSERT_EQUAL_UINT(NONE, blockedByRowAddrDepReqQ[ch][way].tailReq);
			TEST_ASSERT_EQUAL_UINT(0, blockedByRowAddrDepReqQ[ch][way].reqCnt);
			TEST_ASSERT_EQUAL_UINT(NONE, nandReqQ[ch][way].headReq);
			TEST_ASSERT_EQUAL_UINT(NONE, nandReqQ[ch][way].tailReq);
			TEST_ASSERT_EQUAL_UINT(0, nandReqQ[ch][way].reqCnt);
		}
	TEST_ASSERT_EQUAL_UINT(0, notCompletedNandReqCnt);
	TEST_ASSERT_EQUAL_UINT(0, blockedReqCnt);
}

static void test_reinit_after_use_restores_pristine_pool(void)
{
	unsigned int a = GetFromFreeReqQ();
	unsigned int b = GetFromFreeReqQ();

	PutToSliceReqQ(a);
	PutToNandReqQ(b, 0, 0);
	PutToFreeReqQ(GetFromFreeReqQ());

	InitReqPool();

	TEST_ASSERT_EQUAL_UINT(POOL_SIZE, freeReqQ.reqCnt);
	TEST_ASSERT_EQUAL_UINT(0, freeReqQ.headReq);
	TEST_ASSERT_EQUAL_UINT(POOL_SIZE - 1, freeReqQ.tailReq);
	TEST_ASSERT_EQUAL_UINT(0, sliceReqQ.reqCnt);
	TEST_ASSERT_EQUAL_UINT(0, nandReqQ[0][0].reqCnt);
	TEST_ASSERT_EQUAL_UINT(0, notCompletedNandReqCnt);
	TEST_ASSERT_EQUAL_UINT(POOL_SIZE, count_slots_with_queue_type(REQ_QUEUE_TYPE_FREE));
	assert_free_list_consistent();
}

/* ------------------------------------------------------------- free req queue */

static void test_get_from_free_pops_head_in_fifo_order(void)
{
	unsigned int i;

	for (i = 0; i < 5; i++) {
		unsigned int tag = GetFromFreeReqQ();

		TEST_ASSERT_EQUAL_UINT(i, tag);
		TEST_ASSERT_EQUAL_UINT(REQ_QUEUE_TYPE_NONE, req(tag)->reqQueueType);
		TEST_ASSERT_EQUAL_UINT(i + 1, freeReqQ.headReq);
		TEST_ASSERT_EQUAL_UINT(NONE, req(i + 1)->prevReq);
		TEST_ASSERT_EQUAL_UINT(POOL_SIZE - 1 - i, freeReqQ.reqCnt);
	}
	TEST_ASSERT_EQUAL_UINT(0, syncAvailFreeReqCalls);
	assert_free_list_consistent();
}

static void test_put_to_free_appends_at_tail(void)
{
	unsigned int a = GetFromFreeReqQ();
	unsigned int b = GetFromFreeReqQ();
	unsigned int oldTail = freeReqQ.tailReq;

	PutToFreeReqQ(b);
	TEST_ASSERT_EQUAL_UINT(b, freeReqQ.tailReq);
	TEST_ASSERT_EQUAL_UINT(b, req(oldTail)->nextReq);
	TEST_ASSERT_EQUAL_UINT(oldTail, req(b)->prevReq);
	TEST_ASSERT_EQUAL_UINT(NONE, req(b)->nextReq);

	PutToFreeReqQ(a);
	TEST_ASSERT_EQUAL_UINT(a, freeReqQ.tailReq);
	TEST_ASSERT_EQUAL_UINT(a, req(b)->nextReq);
	TEST_ASSERT_EQUAL_UINT(b, req(a)->prevReq);
	TEST_ASSERT_EQUAL_UINT(POOL_SIZE, freeReqQ.reqCnt);
	assert_free_list_consistent();
}

static void test_draining_pool_leaves_empty_free_queue(void)
{
	unsigned int i;

	for (i = 0; i < POOL_SIZE; i++)
		TEST_ASSERT_EQUAL_UINT(i, GetFromFreeReqQ());

	TEST_ASSERT_EQUAL_UINT(0, freeReqQ.reqCnt);
	TEST_ASSERT_EQUAL_UINT(NONE, freeReqQ.headReq);
	TEST_ASSERT_EQUAL_UINT(NONE, freeReqQ.tailReq);
	TEST_ASSERT_EQUAL_UINT(0, count_slots_with_queue_type(REQ_QUEUE_TYPE_FREE));
	TEST_ASSERT_EQUAL_UINT(0, syncAvailFreeReqCalls);
}

static void test_get_from_exhausted_pool_syncs_then_takes_released_slot(void)
{
	unsigned int i, tag;

	for (i = 0; i < POOL_SIZE; i++)
		GetFromFreeReqQ();

	syncAvailFreeReqRefillTag = 7;
	tag = GetFromFreeReqQ();

	TEST_ASSERT_EQUAL_UINT(1, syncAvailFreeReqCalls);
	TEST_ASSERT_EQUAL_UINT(7, tag);
	TEST_ASSERT_EQUAL_UINT(REQ_QUEUE_TYPE_NONE, req(7)->reqQueueType);
	TEST_ASSERT_EQUAL_UINT(0, freeReqQ.reqCnt);
	TEST_ASSERT_EQUAL_UINT(NONE, freeReqQ.headReq);
	TEST_ASSERT_EQUAL_UINT(NONE, freeReqQ.tailReq);
}

static void test_put_to_empty_free_queue_makes_slot_head_and_tail(void)
{
	unsigned int i;

	for (i = 0; i < POOL_SIZE; i++)
		GetFromFreeReqQ();

	PutToFreeReqQ(3);

	TEST_ASSERT_EQUAL_UINT(3, freeReqQ.headReq);
	TEST_ASSERT_EQUAL_UINT(3, freeReqQ.tailReq);
	TEST_ASSERT_EQUAL_UINT(1, freeReqQ.reqCnt);
	TEST_ASSERT_EQUAL_UINT(NONE, req(3)->prevReq);
	TEST_ASSERT_EQUAL_UINT(NONE, req(3)->nextReq);
	TEST_ASSERT_EQUAL_UINT(REQ_QUEUE_TYPE_FREE, req(3)->reqQueueType);

	TEST_ASSERT_EQUAL_UINT(3, GetFromFreeReqQ());
	TEST_ASSERT_EQUAL_UINT(0, syncAvailFreeReqCalls);
}

static void test_free_list_stays_consistent_after_mixed_alloc_release(void)
{
	unsigned int tags[64];
	unsigned int i, round;

	for (round = 0; round < 4; round++) {
		for (i = 0; i < 64; i++)
			tags[i] = GetFromFreeReqQ();
		/* release in a scrambled order: evens backwards, then odds forwards */
		for (i = 64; i > 0; i -= 2)
			PutToFreeReqQ(tags[i - 2]);
		for (i = 1; i < 64; i += 2)
			PutToFreeReqQ(tags[i]);
		TEST_ASSERT_EQUAL_UINT(POOL_SIZE, freeReqQ.reqCnt);
		assert_free_list_consistent();
	}
	TEST_ASSERT_EQUAL_UINT(POOL_SIZE, count_slots_with_queue_type(REQ_QUEUE_TYPE_FREE));
	TEST_ASSERT_EQUAL_UINT(0, syncAvailFreeReqCalls);
}

static void test_full_drain_and_refill_never_duplicates_a_slot(void)
{
	static unsigned char seen[POOL_SIZE];
	unsigned int i;

	for (i = 0; i < POOL_SIZE; i++)
		PutToFreeReqQ(GetFromFreeReqQ()); /* rotate the whole pool once */

	for (i = 0; i < POOL_SIZE; i++) {
		unsigned int tag = GetFromFreeReqQ();

		TEST_ASSERT_TRUE(tag < POOL_SIZE);
		TEST_ASSERT_FALSE_MESSAGE(seen[tag], "slot handed out twice");
		seen[tag] = 1;
	}
	TEST_ASSERT_EQUAL_UINT(0, freeReqQ.reqCnt);
}

/* ------------------------------------------------------------ slice req queue */

static void test_get_from_empty_slice_queue_fails(void)
{
	TEST_ASSERT_EQUAL_UINT(REQ_SLOT_TAG_FAIL, GetFromSliceReqQ());
	TEST_ASSERT_EQUAL_UINT(0, sliceReqQ.reqCnt);
}

static void test_slice_queue_is_fifo_and_tracks_head_tail(void)
{
	unsigned int a = GetFromFreeReqQ();
	unsigned int b = GetFromFreeReqQ();
	unsigned int c = GetFromFreeReqQ();

	PutToSliceReqQ(a);
	TEST_ASSERT_EQUAL_UINT(a, sliceReqQ.headReq);
	TEST_ASSERT_EQUAL_UINT(a, sliceReqQ.tailReq);
	TEST_ASSERT_EQUAL_UINT(NONE, req(a)->prevReq);
	TEST_ASSERT_EQUAL_UINT(NONE, req(a)->nextReq);
	TEST_ASSERT_EQUAL_UINT(REQ_QUEUE_TYPE_SLICE, req(a)->reqQueueType);

	PutToSliceReqQ(b);
	PutToSliceReqQ(c);
	TEST_ASSERT_EQUAL_UINT(3, sliceReqQ.reqCnt);
	TEST_ASSERT_EQUAL_UINT(a, sliceReqQ.headReq);
	TEST_ASSERT_EQUAL_UINT(c, sliceReqQ.tailReq);
	TEST_ASSERT_EQUAL_UINT(3, walk_list(sliceReqQ.headReq, sliceReqQ.tailReq));

	TEST_ASSERT_EQUAL_UINT(a, GetFromSliceReqQ());
	TEST_ASSERT_EQUAL_UINT(REQ_QUEUE_TYPE_NONE, req(a)->reqQueueType);
	TEST_ASSERT_EQUAL_UINT(b, sliceReqQ.headReq);
	TEST_ASSERT_EQUAL_UINT(NONE, req(b)->prevReq);

	TEST_ASSERT_EQUAL_UINT(b, GetFromSliceReqQ());
	TEST_ASSERT_EQUAL_UINT(c, GetFromSliceReqQ());
	TEST_ASSERT_EQUAL_UINT(NONE, sliceReqQ.headReq);
	TEST_ASSERT_EQUAL_UINT(NONE, sliceReqQ.tailReq);
	TEST_ASSERT_EQUAL_UINT(0, sliceReqQ.reqCnt);
	TEST_ASSERT_EQUAL_UINT(REQ_SLOT_TAG_FAIL, GetFromSliceReqQ());
}

static void test_slice_queue_does_not_touch_free_count(void)
{
	unsigned int a = GetFromFreeReqQ();

	PutToSliceReqQ(a);
	TEST_ASSERT_EQUAL_UINT(POOL_SIZE - 1, freeReqQ.reqCnt);
	GetFromSliceReqQ();
	TEST_ASSERT_EQUAL_UINT(POOL_SIZE - 1, freeReqQ.reqCnt);
	PutToFreeReqQ(a);
	TEST_ASSERT_EQUAL_UINT(POOL_SIZE, freeReqQ.reqCnt);
	assert_free_list_consistent();
}

/* ------------------------------------------- blocked-by-buffer-dependency queue */

static void test_buf_dep_put_appends_and_bumps_blocked_count(void)
{
	unsigned int a = GetFromFreeReqQ();
	unsigned int b = GetFromFreeReqQ();

	PutToBlockedByBufDepReqQ(a);
	TEST_ASSERT_EQUAL_UINT(a, blockedByBufDepReqQ.headReq);
	TEST_ASSERT_EQUAL_UINT(a, blockedByBufDepReqQ.tailReq);
	TEST_ASSERT_EQUAL_UINT(REQ_QUEUE_TYPE_BLOCKED_BY_BUF_DEP, req(a)->reqQueueType);
	TEST_ASSERT_EQUAL_UINT(1, blockedReqCnt);

	PutToBlockedByBufDepReqQ(b);
	TEST_ASSERT_EQUAL_UINT(a, blockedByBufDepReqQ.headReq);
	TEST_ASSERT_EQUAL_UINT(b, blockedByBufDepReqQ.tailReq);
	TEST_ASSERT_EQUAL_UINT(b, req(a)->nextReq);
	TEST_ASSERT_EQUAL_UINT(a, req(b)->prevReq);
	TEST_ASSERT_EQUAL_UINT(2, blockedByBufDepReqQ.reqCnt);
	TEST_ASSERT_EQUAL_UINT(2, blockedReqCnt);
}

static void test_buf_dep_selective_get_middle_relinks_neighbours(void)
{
	unsigned int a = GetFromFreeReqQ();
	unsigned int b = GetFromFreeReqQ();
	unsigned int c = GetFromFreeReqQ();

	PutToBlockedByBufDepReqQ(a);
	PutToBlockedByBufDepReqQ(b);
	PutToBlockedByBufDepReqQ(c);

	SelectiveGetFromBlockedByBufDepReqQ(b);

	TEST_ASSERT_EQUAL_UINT(c, req(a)->nextReq);
	TEST_ASSERT_EQUAL_UINT(a, req(c)->prevReq);
	TEST_ASSERT_EQUAL_UINT(a, blockedByBufDepReqQ.headReq);
	TEST_ASSERT_EQUAL_UINT(c, blockedByBufDepReqQ.tailReq);
	TEST_ASSERT_EQUAL_UINT(REQ_QUEUE_TYPE_NONE, req(b)->reqQueueType);
	TEST_ASSERT_EQUAL_UINT(2, blockedByBufDepReqQ.reqCnt);
	TEST_ASSERT_EQUAL_UINT(2, blockedReqCnt);
	TEST_ASSERT_EQUAL_UINT(2, walk_list(blockedByBufDepReqQ.headReq, blockedByBufDepReqQ.tailReq));
}

static void test_buf_dep_selective_get_tail_moves_tail_back(void)
{
	unsigned int a = GetFromFreeReqQ();
	unsigned int b = GetFromFreeReqQ();

	PutToBlockedByBufDepReqQ(a);
	PutToBlockedByBufDepReqQ(b);

	SelectiveGetFromBlockedByBufDepReqQ(b);

	TEST_ASSERT_EQUAL_UINT(a, blockedByBufDepReqQ.headReq);
	TEST_ASSERT_EQUAL_UINT(a, blockedByBufDepReqQ.tailReq);
	TEST_ASSERT_EQUAL_UINT(NONE, req(a)->nextReq);
	TEST_ASSERT_EQUAL_UINT(1, blockedByBufDepReqQ.reqCnt);
	TEST_ASSERT_EQUAL_UINT(1, blockedReqCnt);
}

static void test_buf_dep_selective_get_head_moves_head_forward(void)
{
	unsigned int a = GetFromFreeReqQ();
	unsigned int b = GetFromFreeReqQ();

	PutToBlockedByBufDepReqQ(a);
	PutToBlockedByBufDepReqQ(b);

	SelectiveGetFromBlockedByBufDepReqQ(a);

	TEST_ASSERT_EQUAL_UINT(b, blockedByBufDepReqQ.headReq);
	TEST_ASSERT_EQUAL_UINT(b, blockedByBufDepReqQ.tailReq);
	TEST_ASSERT_EQUAL_UINT(NONE, req(b)->prevReq);
	TEST_ASSERT_EQUAL_UINT(1, blockedByBufDepReqQ.reqCnt);
}

static void test_buf_dep_selective_get_only_element_empties_queue(void)
{
	unsigned int a = GetFromFreeReqQ();

	PutToBlockedByBufDepReqQ(a);
	SelectiveGetFromBlockedByBufDepReqQ(a);

	TEST_ASSERT_EQUAL_UINT(NONE, blockedByBufDepReqQ.headReq);
	TEST_ASSERT_EQUAL_UINT(NONE, blockedByBufDepReqQ.tailReq);
	TEST_ASSERT_EQUAL_UINT(0, blockedByBufDepReqQ.reqCnt);
	TEST_ASSERT_EQUAL_UINT(0, blockedReqCnt);
	TEST_ASSERT_EQUAL_UINT(REQ_QUEUE_TYPE_NONE, req(a)->reqQueueType);
}

static void test_buf_dep_selective_get_with_none_tag_asserts(void)
{
	FW_EXPECT_ASSERT(SelectiveGetFromBlockedByBufDepReqQ(NONE));
}

/* ------------------------------------------ blocked-by-row-address-dependency Q */

static void test_row_dep_queues_are_independent_per_die(void)
{
	unsigned int a = GetFromFreeReqQ();
	unsigned int b = GetFromFreeReqQ();
	unsigned int lastCh = USER_CHANNELS - 1;
	unsigned int lastWay = USER_WAYS - 1;

	PutToBlockedByRowAddrDepReqQ(a, 0, 0);
	PutToBlockedByRowAddrDepReqQ(b, lastCh, lastWay);

	TEST_ASSERT_EQUAL_UINT(a, blockedByRowAddrDepReqQ[0][0].headReq);
	TEST_ASSERT_EQUAL_UINT(a, blockedByRowAddrDepReqQ[0][0].tailReq);
	TEST_ASSERT_EQUAL_UINT(1, blockedByRowAddrDepReqQ[0][0].reqCnt);
	TEST_ASSERT_EQUAL_UINT(b, blockedByRowAddrDepReqQ[lastCh][lastWay].headReq);
	TEST_ASSERT_EQUAL_UINT(1, blockedByRowAddrDepReqQ[lastCh][lastWay].reqCnt);
	TEST_ASSERT_EQUAL_UINT(REQ_QUEUE_TYPE_BLOCKED_BY_ROW_ADDR_DEP, req(a)->reqQueueType);
	TEST_ASSERT_EQUAL_UINT(2, blockedReqCnt);
	if (USER_WAYS > 1)
		TEST_ASSERT_EQUAL_UINT(NONE, blockedByRowAddrDepReqQ[0][1].headReq);
}

static void test_row_dep_put_appends_at_tail(void)
{
	unsigned int a = GetFromFreeReqQ();
	unsigned int b = GetFromFreeReqQ();
	unsigned int c = GetFromFreeReqQ();

	PutToBlockedByRowAddrDepReqQ(a, 0, 1);
	PutToBlockedByRowAddrDepReqQ(b, 0, 1);
	PutToBlockedByRowAddrDepReqQ(c, 0, 1);

	TEST_ASSERT_EQUAL_UINT(a, blockedByRowAddrDepReqQ[0][1].headReq);
	TEST_ASSERT_EQUAL_UINT(c, blockedByRowAddrDepReqQ[0][1].tailReq);
	TEST_ASSERT_EQUAL_UINT(3, blockedByRowAddrDepReqQ[0][1].reqCnt);
	TEST_ASSERT_EQUAL_UINT(3, walk_list(a, c));
	TEST_ASSERT_EQUAL_UINT(3, blockedReqCnt);
}

static void test_row_dep_selective_get_covers_middle_tail_head_and_last(void)
{
	unsigned int a = GetFromFreeReqQ();
	unsigned int b = GetFromFreeReqQ();
	unsigned int c = GetFromFreeReqQ();
	unsigned int d = GetFromFreeReqQ();

	PutToBlockedByRowAddrDepReqQ(a, 1 % USER_CHANNELS, 2);
	PutToBlockedByRowAddrDepReqQ(b, 1 % USER_CHANNELS, 2);
	PutToBlockedByRowAddrDepReqQ(c, 1 % USER_CHANNELS, 2);
	PutToBlockedByRowAddrDepReqQ(d, 1 % USER_CHANNELS, 2);

	/* middle */
	SelectiveGetFromBlockedByRowAddrDepReqQ(b, 1 % USER_CHANNELS, 2);
	TEST_ASSERT_EQUAL_UINT(c, req(a)->nextReq);
	TEST_ASSERT_EQUAL_UINT(a, req(c)->prevReq);
	TEST_ASSERT_EQUAL_UINT(3, blockedByRowAddrDepReqQ[1 % USER_CHANNELS][2].reqCnt);

	/* tail */
	SelectiveGetFromBlockedByRowAddrDepReqQ(d, 1 % USER_CHANNELS, 2);
	TEST_ASSERT_EQUAL_UINT(c, blockedByRowAddrDepReqQ[1 % USER_CHANNELS][2].tailReq);
	TEST_ASSERT_EQUAL_UINT(NONE, req(c)->nextReq);

	/* head */
	SelectiveGetFromBlockedByRowAddrDepReqQ(a, 1 % USER_CHANNELS, 2);
	TEST_ASSERT_EQUAL_UINT(c, blockedByRowAddrDepReqQ[1 % USER_CHANNELS][2].headReq);
	TEST_ASSERT_EQUAL_UINT(NONE, req(c)->prevReq);

	/* last */
	SelectiveGetFromBlockedByRowAddrDepReqQ(c, 1 % USER_CHANNELS, 2);
	TEST_ASSERT_EQUAL_UINT(NONE, blockedByRowAddrDepReqQ[1 % USER_CHANNELS][2].headReq);
	TEST_ASSERT_EQUAL_UINT(NONE, blockedByRowAddrDepReqQ[1 % USER_CHANNELS][2].tailReq);
	TEST_ASSERT_EQUAL_UINT(0, blockedByRowAddrDepReqQ[1 % USER_CHANNELS][2].reqCnt);
	TEST_ASSERT_EQUAL_UINT(0, blockedReqCnt);
	TEST_ASSERT_EQUAL_UINT(REQ_QUEUE_TYPE_NONE, req(c)->reqQueueType);
}

static void test_row_dep_selective_get_with_none_tag_asserts(void)
{
	FW_EXPECT_ASSERT(SelectiveGetFromBlockedByRowAddrDepReqQ(NONE, 0, 0));
}

static void test_blocked_req_cnt_is_shared_between_buf_and_row_dep_queues(void)
{
	unsigned int a = GetFromFreeReqQ();
	unsigned int b = GetFromFreeReqQ();

	PutToBlockedByBufDepReqQ(a);
	PutToBlockedByRowAddrDepReqQ(b, 0, 0);
	TEST_ASSERT_EQUAL_UINT(2, blockedReqCnt);

	SelectiveGetFromBlockedByRowAddrDepReqQ(b, 0, 0);
	TEST_ASSERT_EQUAL_UINT(1, blockedReqCnt);
	SelectiveGetFromBlockedByBufDepReqQ(a);
	TEST_ASSERT_EQUAL_UINT(0, blockedReqCnt);
}

/* --------------------------------------------------------- NVMe DMA req queue */

static void test_nvme_dma_put_appends_and_marks_type(void)
{
	unsigned int a = GetFromFreeReqQ();
	unsigned int b = GetFromFreeReqQ();

	PutToNvmeDmaReqQ(a);
	TEST_ASSERT_EQUAL_UINT(a, nvmeDmaReqQ.headReq);
	TEST_ASSERT_EQUAL_UINT(a, nvmeDmaReqQ.tailReq);
	TEST_ASSERT_EQUAL_UINT(REQ_QUEUE_TYPE_NVME_DMA, req(a)->reqQueueType);

	PutToNvmeDmaReqQ(b);
	TEST_ASSERT_EQUAL_UINT(b, nvmeDmaReqQ.tailReq);
	TEST_ASSERT_EQUAL_UINT(b, req(a)->nextReq);
	TEST_ASSERT_EQUAL_UINT(a, req(b)->prevReq);
	TEST_ASSERT_EQUAL_UINT(2, nvmeDmaReqQ.reqCnt);
	TEST_ASSERT_EQUAL_UINT(0, releaseBlockedCalls);
}

static void test_nvme_dma_selective_get_returns_slot_to_free_and_releases_dependents(void)
{
	unsigned int a = GetFromFreeReqQ();
	unsigned int b = GetFromFreeReqQ();
	unsigned int c = GetFromFreeReqQ();

	PutToNvmeDmaReqQ(a);
	PutToNvmeDmaReqQ(b);
	PutToNvmeDmaReqQ(c);

	/* middle */
	SelectiveGetFromNvmeDmaReqQ(b);
	TEST_ASSERT_EQUAL_UINT(c, req(a)->nextReq);
	TEST_ASSERT_EQUAL_UINT(a, req(c)->prevReq);
	TEST_ASSERT_EQUAL_UINT(2, nvmeDmaReqQ.reqCnt);
	TEST_ASSERT_EQUAL_UINT(REQ_QUEUE_TYPE_FREE, req(b)->reqQueueType);
	TEST_ASSERT_EQUAL_UINT(b, freeReqQ.tailReq);
	TEST_ASSERT_EQUAL_UINT(POOL_SIZE - 2, freeReqQ.reqCnt);
	TEST_ASSERT_EQUAL_UINT(1, releaseBlockedCalls);
	TEST_ASSERT_EQUAL_UINT(b, releaseBlockedLastTag);

	/* tail */
	SelectiveGetFromNvmeDmaReqQ(c);
	TEST_ASSERT_EQUAL_UINT(a, nvmeDmaReqQ.tailReq);
	TEST_ASSERT_EQUAL_UINT(NONE, req(a)->nextReq);
	TEST_ASSERT_EQUAL_UINT(c, releaseBlockedLastTag);

	/* last */
	SelectiveGetFromNvmeDmaReqQ(a);
	TEST_ASSERT_EQUAL_UINT(NONE, nvmeDmaReqQ.headReq);
	TEST_ASSERT_EQUAL_UINT(NONE, nvmeDmaReqQ.tailReq);
	TEST_ASSERT_EQUAL_UINT(0, nvmeDmaReqQ.reqCnt);
	TEST_ASSERT_EQUAL_UINT(3, releaseBlockedCalls);
	TEST_ASSERT_EQUAL_UINT(POOL_SIZE, freeReqQ.reqCnt);
	assert_free_list_consistent();
}

static void test_nvme_dma_selective_get_head_advances_head(void)
{
	unsigned int a = GetFromFreeReqQ();
	unsigned int b = GetFromFreeReqQ();

	PutToNvmeDmaReqQ(a);
	PutToNvmeDmaReqQ(b);

	SelectiveGetFromNvmeDmaReqQ(a);

	TEST_ASSERT_EQUAL_UINT(b, nvmeDmaReqQ.headReq);
	TEST_ASSERT_EQUAL_UINT(NONE, req(b)->prevReq);
	TEST_ASSERT_EQUAL_UINT(1, nvmeDmaReqQ.reqCnt);
	TEST_ASSERT_EQUAL_UINT(a, releaseBlockedLastTag);
}

/* ------------------------------------------------------------- NAND req queue */

static void test_nand_put_is_per_die_and_counts_outstanding(void)
{
	unsigned int a = GetFromFreeReqQ();
	unsigned int b = GetFromFreeReqQ();
	unsigned int c = GetFromFreeReqQ();
	unsigned int lastCh = USER_CHANNELS - 1;
	unsigned int lastWay = USER_WAYS - 1;

	PutToNandReqQ(a, 0, 0);
	PutToNandReqQ(b, 0, 0);
	PutToNandReqQ(c, lastCh, lastWay);

	TEST_ASSERT_EQUAL_UINT(a, nandReqQ[0][0].headReq);
	TEST_ASSERT_EQUAL_UINT(b, nandReqQ[0][0].tailReq);
	TEST_ASSERT_EQUAL_UINT(b, req(a)->nextReq);
	TEST_ASSERT_EQUAL_UINT(a, req(b)->prevReq);
	TEST_ASSERT_EQUAL_UINT(2, nandReqQ[0][0].reqCnt);
	TEST_ASSERT_EQUAL_UINT(c, nandReqQ[lastCh][lastWay].headReq);
	TEST_ASSERT_EQUAL_UINT(c, nandReqQ[lastCh][lastWay].tailReq);
	TEST_ASSERT_EQUAL_UINT(1, nandReqQ[lastCh][lastWay].reqCnt);
	TEST_ASSERT_EQUAL_UINT(REQ_QUEUE_TYPE_NAND, req(c)->reqQueueType);
	TEST_ASSERT_EQUAL_UINT(3, notCompletedNandReqCnt);
	TEST_ASSERT_EQUAL_UINT(0, blockedReqCnt);
}

static void test_nand_get_pops_head_frees_slot_and_releases_dependents(void)
{
	unsigned int a = GetFromFreeReqQ();
	unsigned int b = GetFromFreeReqQ();

	PutToNandReqQ(a, 0, 3);
	PutToNandReqQ(b, 0, 3);

	GetFromNandReqQ(0, 3, 0, 0);

	TEST_ASSERT_EQUAL_UINT(b, nandReqQ[0][3].headReq);
	TEST_ASSERT_EQUAL_UINT(NONE, req(b)->prevReq);
	TEST_ASSERT_EQUAL_UINT(1, nandReqQ[0][3].reqCnt);
	TEST_ASSERT_EQUAL_UINT(1, notCompletedNandReqCnt);
	TEST_ASSERT_EQUAL_UINT(REQ_QUEUE_TYPE_FREE, req(a)->reqQueueType);
	TEST_ASSERT_EQUAL_UINT(a, freeReqQ.tailReq);
	TEST_ASSERT_EQUAL_UINT(1, releaseBlockedCalls);
	TEST_ASSERT_EQUAL_UINT(a, releaseBlockedLastTag);

	GetFromNandReqQ(0, 3, 0, 0);

	TEST_ASSERT_EQUAL_UINT(NONE, nandReqQ[0][3].headReq);
	TEST_ASSERT_EQUAL_UINT(NONE, nandReqQ[0][3].tailReq);
	TEST_ASSERT_EQUAL_UINT(0, nandReqQ[0][3].reqCnt);
	TEST_ASSERT_EQUAL_UINT(0, notCompletedNandReqCnt);
	TEST_ASSERT_EQUAL_UINT(2, releaseBlockedCalls);
	TEST_ASSERT_EQUAL_UINT(b, releaseBlockedLastTag);
	TEST_ASSERT_EQUAL_UINT(POOL_SIZE, freeReqQ.reqCnt);
	assert_free_list_consistent();
}

static void test_nand_get_from_empty_die_queue_asserts(void)
{
	FW_EXPECT_ASSERT(GetFromNandReqQ(0, 0, 0, 0));
}

static void test_nand_get_on_one_die_leaves_other_dies_untouched(void)
{
	unsigned int a = GetFromFreeReqQ();
	unsigned int b = GetFromFreeReqQ();

	PutToNandReqQ(a, 0, 0);
	PutToNandReqQ(b, 0, 1);

	GetFromNandReqQ(0, 0, 0, 0);

	TEST_ASSERT_EQUAL_UINT(NONE, nandReqQ[0][0].headReq);
	TEST_ASSERT_EQUAL_UINT(b, nandReqQ[0][1].headReq);
	TEST_ASSERT_EQUAL_UINT(1, nandReqQ[0][1].reqCnt);
	TEST_ASSERT_EQUAL_UINT(1, notCompletedNandReqCnt);
}

/* -------------------------------------------------------- mixed-queue scenario */

static void test_slots_spread_over_all_queues_all_return_to_free(void)
{
	unsigned int t[6];
	unsigned int i;

	for (i = 0; i < 6; i++)
		t[i] = GetFromFreeReqQ();
	TEST_ASSERT_EQUAL_UINT(POOL_SIZE - 6, freeReqQ.reqCnt);

	PutToSliceReqQ(t[0]);
	PutToBlockedByBufDepReqQ(t[1]);
	PutToBlockedByRowAddrDepReqQ(t[2], 0, 0);
	PutToNvmeDmaReqQ(t[3]);
	PutToNandReqQ(t[4], 0, 0);
	PutToNandReqQ(t[5], 0, 0);

	TEST_ASSERT_EQUAL_UINT(2, blockedReqCnt);
	TEST_ASSERT_EQUAL_UINT(2, notCompletedNandReqCnt);
	TEST_ASSERT_EQUAL_UINT(POOL_SIZE - 6, count_slots_with_queue_type(REQ_QUEUE_TYPE_FREE));
	TEST_ASSERT_EQUAL_UINT(1, count_slots_with_queue_type(REQ_QUEUE_TYPE_SLICE));
	TEST_ASSERT_EQUAL_UINT(1, count_slots_with_queue_type(REQ_QUEUE_TYPE_BLOCKED_BY_BUF_DEP));
	TEST_ASSERT_EQUAL_UINT(1, count_slots_with_queue_type(REQ_QUEUE_TYPE_BLOCKED_BY_ROW_ADDR_DEP));
	TEST_ASSERT_EQUAL_UINT(1, count_slots_with_queue_type(REQ_QUEUE_TYPE_NVME_DMA));
	TEST_ASSERT_EQUAL_UINT(2, count_slots_with_queue_type(REQ_QUEUE_TYPE_NAND));

	PutToFreeReqQ(GetFromSliceReqQ());
	SelectiveGetFromBlockedByBufDepReqQ(t[1]);
	PutToFreeReqQ(t[1]);
	SelectiveGetFromBlockedByRowAddrDepReqQ(t[2], 0, 0);
	PutToFreeReqQ(t[2]);
	SelectiveGetFromNvmeDmaReqQ(t[3]);
	GetFromNandReqQ(0, 0, 0, 0);
	GetFromNandReqQ(0, 0, 0, 0);

	TEST_ASSERT_EQUAL_UINT(POOL_SIZE, freeReqQ.reqCnt);
	TEST_ASSERT_EQUAL_UINT(POOL_SIZE, count_slots_with_queue_type(REQ_QUEUE_TYPE_FREE));
	TEST_ASSERT_EQUAL_UINT(0, blockedReqCnt);
	TEST_ASSERT_EQUAL_UINT(0, notCompletedNandReqCnt);
	TEST_ASSERT_EQUAL_UINT(3, releaseBlockedCalls);
	assert_free_list_consistent();
}

int main(void)
{
	UNITY_BEGIN();

	RUN_TEST(test_init_links_pool_as_single_free_chain);
	RUN_TEST(test_init_empties_every_other_queue_and_counters);
	RUN_TEST(test_reinit_after_use_restores_pristine_pool);

	RUN_TEST(test_get_from_free_pops_head_in_fifo_order);
	RUN_TEST(test_put_to_free_appends_at_tail);
	RUN_TEST(test_draining_pool_leaves_empty_free_queue);
	RUN_TEST(test_get_from_exhausted_pool_syncs_then_takes_released_slot);
	RUN_TEST(test_put_to_empty_free_queue_makes_slot_head_and_tail);
	RUN_TEST(test_free_list_stays_consistent_after_mixed_alloc_release);
	RUN_TEST(test_full_drain_and_refill_never_duplicates_a_slot);

	RUN_TEST(test_get_from_empty_slice_queue_fails);
	RUN_TEST(test_slice_queue_is_fifo_and_tracks_head_tail);
	RUN_TEST(test_slice_queue_does_not_touch_free_count);

	RUN_TEST(test_buf_dep_put_appends_and_bumps_blocked_count);
	RUN_TEST(test_buf_dep_selective_get_middle_relinks_neighbours);
	RUN_TEST(test_buf_dep_selective_get_tail_moves_tail_back);
	RUN_TEST(test_buf_dep_selective_get_head_moves_head_forward);
	RUN_TEST(test_buf_dep_selective_get_only_element_empties_queue);
	RUN_TEST(test_buf_dep_selective_get_with_none_tag_asserts);

	RUN_TEST(test_row_dep_queues_are_independent_per_die);
	RUN_TEST(test_row_dep_put_appends_at_tail);
	RUN_TEST(test_row_dep_selective_get_covers_middle_tail_head_and_last);
	RUN_TEST(test_row_dep_selective_get_with_none_tag_asserts);
	RUN_TEST(test_blocked_req_cnt_is_shared_between_buf_and_row_dep_queues);

	RUN_TEST(test_nvme_dma_put_appends_and_marks_type);
	RUN_TEST(test_nvme_dma_selective_get_returns_slot_to_free_and_releases_dependents);
	RUN_TEST(test_nvme_dma_selective_get_head_advances_head);

	RUN_TEST(test_nand_put_is_per_die_and_counts_outstanding);
	RUN_TEST(test_nand_get_pops_head_frees_slot_and_releases_dependents);
	RUN_TEST(test_nand_get_from_empty_die_queue_asserts);
	RUN_TEST(test_nand_get_on_one_die_leaves_other_dies_untouched);

	RUN_TEST(test_slots_spread_over_all_queues_all_return_to_free);

	return UNITY_END();
}
