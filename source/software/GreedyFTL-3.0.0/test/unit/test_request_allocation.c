/* Unit tests for request_allocation.c. */
#include "unity.h"

#include "fw_test.h"
#include "memory_map.h"

#define TEST_CH 0
#define TEST_WAY 0

void setUp(void)
{
	fw_test_reset();
	InitReqPool();
}

void tearDown(void) {}

static SSD_REQ_FORMAT *req(unsigned int reqSlotTag)
{
	return &reqPoolPtr->reqPool[reqSlotTag];
}

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
	TEST_ASSERT_EQUAL_UINT(REQ_QUEUE_TYPE_FREE, req(reqSlotTag)->reqQueueType);
}

static void test_init_clears_every_queue_and_counter(void)
{
	unsigned int chNo, wayNo;

	TEST_ASSERT_EQUAL_UINT(0, sliceReqQ.reqCnt);
	TEST_ASSERT_EQUAL_UINT(REQ_SLOT_TAG_NONE, sliceReqQ.headReq);
	TEST_ASSERT_EQUAL_UINT(0, blockedByBufDepReqQ.reqCnt);
	TEST_ASSERT_EQUAL_UINT(0, nvmeDmaReqQ.reqCnt);
	TEST_ASSERT_EQUAL_UINT(0, notCompletedNandReqCnt);
	TEST_ASSERT_EQUAL_UINT(0, blockedReqCnt);
	for (chNo = 0; chNo < USER_CHANNELS; chNo++)
		for (wayNo = 0; wayNo < USER_WAYS; wayNo++) {
			TEST_ASSERT_EQUAL_UINT(0, nandReqQ[chNo][wayNo].reqCnt);
			TEST_ASSERT_EQUAL_UINT(0, blockedByRowAddrDepReqQ[chNo][wayNo].reqCnt);
		}
	TEST_ASSERT_EQUAL_UINT(REQ_SLOT_TAG_NONE, req(0)->prevBlockingReq);
	TEST_ASSERT_EQUAL_UINT(REQ_SLOT_TAG_NONE, req(0)->nextBlockingReq);
}

static void test_free_queue_hands_out_slots_in_order_until_empty(void)
{
	unsigned int i;

	for (i = 0; i < AVAILABLE_OUNTSTANDING_REQ_COUNT; i++)
		TEST_ASSERT_EQUAL_UINT(i, GetFromFreeReqQ());

	TEST_ASSERT_EQUAL_UINT(0, freeReqQ.reqCnt);
	TEST_ASSERT_EQUAL_UINT(REQ_SLOT_TAG_NONE, freeReqQ.headReq);
	TEST_ASSERT_EQUAL_UINT(REQ_SLOT_TAG_NONE, freeReqQ.tailReq);
}

static void test_released_slot_is_appended_to_free_queue_tail(void)
{
	unsigned int first = GetFromFreeReqQ();
	unsigned int tailBefore = freeReqQ.tailReq;

	PutToFreeReqQ(first);

	TEST_ASSERT_EQUAL_UINT(first, freeReqQ.tailReq);
	TEST_ASSERT_EQUAL_UINT(first, req(tailBefore)->nextReq);
	TEST_ASSERT_EQUAL_UINT(tailBefore, req(first)->prevReq);
	TEST_ASSERT_EQUAL_UINT(REQ_SLOT_TAG_NONE, req(first)->nextReq);
}

static void test_release_into_empty_free_queue_makes_slot_head_and_tail(void)
{
	unsigned int i;

	for (i = 0; i < AVAILABLE_OUNTSTANDING_REQ_COUNT; i++)
		GetFromFreeReqQ();

	PutToFreeReqQ(5);

	TEST_ASSERT_EQUAL_UINT(5, freeReqQ.headReq);
	TEST_ASSERT_EQUAL_UINT(5, freeReqQ.tailReq);
	TEST_ASSERT_EQUAL_UINT(1, freeReqQ.reqCnt);
	TEST_ASSERT_EQUAL_UINT(5, GetFromFreeReqQ());
	TEST_ASSERT_EQUAL_UINT(REQ_SLOT_TAG_NONE, freeReqQ.headReq);
}

static void test_slice_queue_is_fifo_and_reports_empty(void)
{
	unsigned int a = GetFromFreeReqQ();
	unsigned int b = GetFromFreeReqQ();

	TEST_ASSERT_EQUAL_UINT(REQ_SLOT_TAG_FAIL, GetFromSliceReqQ());

	PutToSliceReqQ(a);
	PutToSliceReqQ(b);
	TEST_ASSERT_EQUAL_UINT(2, sliceReqQ.reqCnt);
	TEST_ASSERT_EQUAL_UINT(REQ_QUEUE_TYPE_SLICE, req(a)->reqQueueType);
	TEST_ASSERT_EQUAL_UINT(b, req(a)->nextReq);

	TEST_ASSERT_EQUAL_UINT(a, GetFromSliceReqQ());
	TEST_ASSERT_EQUAL_UINT(REQ_QUEUE_TYPE_NONE, req(a)->reqQueueType);
	TEST_ASSERT_EQUAL_UINT(b, GetFromSliceReqQ());
	TEST_ASSERT_EQUAL_UINT(0, sliceReqQ.reqCnt);
	TEST_ASSERT_EQUAL_UINT(REQ_SLOT_TAG_NONE, sliceReqQ.tailReq);
	TEST_ASSERT_EQUAL_UINT(REQ_SLOT_TAG_FAIL, GetFromSliceReqQ());
}

static void test_buf_dep_queue_selective_removal_from_any_position(void)
{
	unsigned int a = GetFromFreeReqQ();
	unsigned int b = GetFromFreeReqQ();
	unsigned int c = GetFromFreeReqQ();

	PutToBlockedByBufDepReqQ(a);
	PutToBlockedByBufDepReqQ(b);
	PutToBlockedByBufDepReqQ(c);
	TEST_ASSERT_EQUAL_UINT(3, blockedByBufDepReqQ.reqCnt);
	TEST_ASSERT_EQUAL_UINT(3, blockedReqCnt);
	TEST_ASSERT_EQUAL_UINT(REQ_QUEUE_TYPE_BLOCKED_BY_BUF_DEP, req(b)->reqQueueType);

	SelectiveGetFromBlockedByBufDepReqQ(b); /* middle */
	TEST_ASSERT_EQUAL_UINT(c, req(a)->nextReq);
	TEST_ASSERT_EQUAL_UINT(a, req(c)->prevReq);

	SelectiveGetFromBlockedByBufDepReqQ(c); /* tail */
	TEST_ASSERT_EQUAL_UINT(a, blockedByBufDepReqQ.tailReq);
	TEST_ASSERT_EQUAL_UINT(REQ_SLOT_TAG_NONE, req(a)->nextReq);

	PutToBlockedByBufDepReqQ(b);
	SelectiveGetFromBlockedByBufDepReqQ(a); /* head */
	TEST_ASSERT_EQUAL_UINT(b, blockedByBufDepReqQ.headReq);
	TEST_ASSERT_EQUAL_UINT(REQ_SLOT_TAG_NONE, req(b)->prevReq);

	SelectiveGetFromBlockedByBufDepReqQ(b); /* only */
	TEST_ASSERT_EQUAL_UINT(REQ_SLOT_TAG_NONE, blockedByBufDepReqQ.headReq);
	TEST_ASSERT_EQUAL_UINT(REQ_SLOT_TAG_NONE, blockedByBufDepReqQ.tailReq);
	TEST_ASSERT_EQUAL_UINT(0, blockedByBufDepReqQ.reqCnt);
	TEST_ASSERT_EQUAL_UINT(0, blockedReqCnt);
}

static void test_buf_dep_queue_rejects_none_tag(void)
{
	FW_EXPECT_ASSERT(SelectiveGetFromBlockedByBufDepReqQ(REQ_SLOT_TAG_NONE));
}

static void test_row_dep_queue_selective_removal_from_any_position(void)
{
	BLOCKED_BY_ROW_ADDR_DEPENDENCY_REQUEST_QUEUE *q = &blockedByRowAddrDepReqQ[TEST_CH][TEST_WAY];
	unsigned int a = GetFromFreeReqQ();
	unsigned int b = GetFromFreeReqQ();
	unsigned int c = GetFromFreeReqQ();

	PutToBlockedByRowAddrDepReqQ(a, TEST_CH, TEST_WAY);
	PutToBlockedByRowAddrDepReqQ(b, TEST_CH, TEST_WAY);
	PutToBlockedByRowAddrDepReqQ(c, TEST_CH, TEST_WAY);
	TEST_ASSERT_EQUAL_UINT(3, q->reqCnt);
	TEST_ASSERT_EQUAL_UINT(3, blockedReqCnt);
	TEST_ASSERT_EQUAL_UINT(REQ_QUEUE_TYPE_BLOCKED_BY_ROW_ADDR_DEP, req(a)->reqQueueType);

	SelectiveGetFromBlockedByRowAddrDepReqQ(b, TEST_CH, TEST_WAY);
	TEST_ASSERT_EQUAL_UINT(c, req(a)->nextReq);
	TEST_ASSERT_EQUAL_UINT(a, req(c)->prevReq);

	SelectiveGetFromBlockedByRowAddrDepReqQ(a, TEST_CH, TEST_WAY);
	TEST_ASSERT_EQUAL_UINT(c, q->headReq);

	PutToBlockedByRowAddrDepReqQ(a, TEST_CH, TEST_WAY);
	SelectiveGetFromBlockedByRowAddrDepReqQ(a, TEST_CH, TEST_WAY);
	TEST_ASSERT_EQUAL_UINT(c, q->tailReq);

	SelectiveGetFromBlockedByRowAddrDepReqQ(c, TEST_CH, TEST_WAY);
	TEST_ASSERT_EQUAL_UINT(REQ_SLOT_TAG_NONE, q->headReq);
	TEST_ASSERT_EQUAL_UINT(REQ_SLOT_TAG_NONE, q->tailReq);
	TEST_ASSERT_EQUAL_UINT(0, q->reqCnt);
	TEST_ASSERT_EQUAL_UINT(0, blockedReqCnt);
}

static void test_row_dep_queue_rejects_none_tag(void)
{
	FW_EXPECT_ASSERT(SelectiveGetFromBlockedByRowAddrDepReqQ(REQ_SLOT_TAG_NONE, TEST_CH, TEST_WAY));
}

static void test_nvme_dma_queue_retire_frees_slot_from_any_position(void)
{
	unsigned int a = GetFromFreeReqQ();
	unsigned int b = GetFromFreeReqQ();
	unsigned int c = GetFromFreeReqQ();
	unsigned int freeBefore = freeReqQ.reqCnt;

	req(a)->reqOpt.dataBufFormat = REQ_OPT_DATA_BUF_NONE;
	req(b)->reqOpt.dataBufFormat = REQ_OPT_DATA_BUF_NONE;
	req(c)->reqOpt.dataBufFormat = REQ_OPT_DATA_BUF_NONE;

	PutToNvmeDmaReqQ(a);
	PutToNvmeDmaReqQ(b);
	PutToNvmeDmaReqQ(c);
	TEST_ASSERT_EQUAL_UINT(3, nvmeDmaReqQ.reqCnt);
	TEST_ASSERT_EQUAL_UINT(REQ_QUEUE_TYPE_NVME_DMA, req(b)->reqQueueType);

	SelectiveGetFromNvmeDmaReqQ(b);
	TEST_ASSERT_EQUAL_UINT(c, req(a)->nextReq);
	TEST_ASSERT_EQUAL_UINT(REQ_QUEUE_TYPE_FREE, req(b)->reqQueueType);

	SelectiveGetFromNvmeDmaReqQ(a);
	TEST_ASSERT_EQUAL_UINT(c, nvmeDmaReqQ.headReq);

	SelectiveGetFromNvmeDmaReqQ(c);
	TEST_ASSERT_EQUAL_UINT(REQ_SLOT_TAG_NONE, nvmeDmaReqQ.headReq);
	TEST_ASSERT_EQUAL_UINT(REQ_SLOT_TAG_NONE, nvmeDmaReqQ.tailReq);
	TEST_ASSERT_EQUAL_UINT(0, nvmeDmaReqQ.reqCnt);
	TEST_ASSERT_EQUAL_UINT(freeBefore + 3, freeReqQ.reqCnt);
}

static void test_nvme_dma_queue_tail_removal_relinks_tail(void)
{
	unsigned int a = GetFromFreeReqQ();
	unsigned int b = GetFromFreeReqQ();

	req(a)->reqOpt.dataBufFormat = REQ_OPT_DATA_BUF_NONE;
	req(b)->reqOpt.dataBufFormat = REQ_OPT_DATA_BUF_NONE;
	PutToNvmeDmaReqQ(a);
	PutToNvmeDmaReqQ(b);

	SelectiveGetFromNvmeDmaReqQ(b);

	TEST_ASSERT_EQUAL_UINT(a, nvmeDmaReqQ.tailReq);
	TEST_ASSERT_EQUAL_UINT(REQ_SLOT_TAG_NONE, req(a)->nextReq);
}

static void test_nand_queue_is_fifo_per_way_and_counts_outstanding(void)
{
	unsigned int a = GetFromFreeReqQ();
	unsigned int b = GetFromFreeReqQ();

	req(a)->reqOpt.dataBufFormat = REQ_OPT_DATA_BUF_NONE;
	req(b)->reqOpt.dataBufFormat = REQ_OPT_DATA_BUF_NONE;

	PutToNandReqQ(a, TEST_CH, TEST_WAY);
	PutToNandReqQ(b, TEST_CH, TEST_WAY);
	TEST_ASSERT_EQUAL_UINT(2, nandReqQ[TEST_CH][TEST_WAY].reqCnt);
	TEST_ASSERT_EQUAL_UINT(2, notCompletedNandReqCnt);
	TEST_ASSERT_EQUAL_UINT(REQ_QUEUE_TYPE_NAND, req(a)->reqQueueType);
	TEST_ASSERT_EQUAL_UINT(a, nandReqQ[TEST_CH][TEST_WAY].headReq);
	TEST_ASSERT_EQUAL_UINT(b, nandReqQ[TEST_CH][TEST_WAY].tailReq);

	GetFromNandReqQ(TEST_CH, TEST_WAY, REQ_STATUS_DONE, REQ_CODE_READ);
	TEST_ASSERT_EQUAL_UINT(b, nandReqQ[TEST_CH][TEST_WAY].headReq);
	TEST_ASSERT_EQUAL_UINT(REQ_QUEUE_TYPE_FREE, req(a)->reqQueueType);
	TEST_ASSERT_EQUAL_UINT(1, notCompletedNandReqCnt);

	GetFromNandReqQ(TEST_CH, TEST_WAY, REQ_STATUS_DONE, REQ_CODE_READ);
	TEST_ASSERT_EQUAL_UINT(REQ_SLOT_TAG_NONE, nandReqQ[TEST_CH][TEST_WAY].headReq);
	TEST_ASSERT_EQUAL_UINT(REQ_SLOT_TAG_NONE, nandReqQ[TEST_CH][TEST_WAY].tailReq);
	TEST_ASSERT_EQUAL_UINT(0, notCompletedNandReqCnt);
	TEST_ASSERT_EQUAL_UINT(AVAILABLE_OUNTSTANDING_REQ_COUNT, freeReqQ.reqCnt);
}

static void test_get_from_empty_nand_queue_asserts(void)
{
	FW_EXPECT_ASSERT(GetFromNandReqQ(TEST_CH, TEST_WAY, REQ_STATUS_DONE, REQ_CODE_READ));
}

static void test_completed_nand_req_releases_buffer_blocked_successor(void)
{
	unsigned int first = GetFromFreeReqQ();
	unsigned int second = GetFromFreeReqQ();

	req(first)->reqOpt.dataBufFormat = REQ_OPT_DATA_BUF_NONE;
	req(first)->nextBlockingReq = second;
	req(second)->prevBlockingReq = first;
	req(second)->reqType = REQ_TYPE_NAND;
	req(second)->reqOpt.dataBufFormat = REQ_OPT_DATA_BUF_NONE;
	req(second)->reqOpt.nandAddr = REQ_OPT_NAND_ADDR_VSA;
	req(second)->reqOpt.rowAddrDependencyCheck = REQ_OPT_ROW_ADDR_DEPENDENCY_NONE;
	req(second)->nandInfo.virtualSliceAddr = Vorg2VsaTranslation(0, 1, 0);

	PutToNandReqQ(first, TEST_CH, TEST_WAY);
	PutToBlockedByBufDepReqQ(second);

	GetFromNandReqQ(TEST_CH, TEST_WAY, REQ_STATUS_DONE, REQ_CODE_WRITE);

	TEST_ASSERT_EQUAL_UINT(REQ_SLOT_TAG_NONE, req(second)->prevBlockingReq);
	TEST_ASSERT_EQUAL_UINT(0, blockedByBufDepReqQ.reqCnt);
	TEST_ASSERT_EQUAL_UINT(1, nandReqQ[TEST_CH][TEST_WAY].reqCnt);
	TEST_ASSERT_EQUAL_UINT(second, nandReqQ[TEST_CH][TEST_WAY].headReq);
}

int main(void)
{
	UNITY_BEGIN();
	RUN_TEST(test_smoke_init_req_pool_puts_every_slot_on_free_queue);
	RUN_TEST(test_smoke_alloc_then_release_round_trips_slot);
	RUN_TEST(test_init_clears_every_queue_and_counter);
	RUN_TEST(test_free_queue_hands_out_slots_in_order_until_empty);
	RUN_TEST(test_released_slot_is_appended_to_free_queue_tail);
	RUN_TEST(test_release_into_empty_free_queue_makes_slot_head_and_tail);
	RUN_TEST(test_slice_queue_is_fifo_and_reports_empty);
	RUN_TEST(test_buf_dep_queue_selective_removal_from_any_position);
	RUN_TEST(test_buf_dep_queue_rejects_none_tag);
	RUN_TEST(test_row_dep_queue_selective_removal_from_any_position);
	RUN_TEST(test_row_dep_queue_rejects_none_tag);
	RUN_TEST(test_nvme_dma_queue_retire_frees_slot_from_any_position);
	RUN_TEST(test_nvme_dma_queue_tail_removal_relinks_tail);
	RUN_TEST(test_nand_queue_is_fifo_per_way_and_counts_outstanding);
	RUN_TEST(test_get_from_empty_nand_queue_asserts);
	RUN_TEST(test_completed_nand_req_releases_buffer_blocked_successor);
	return UNITY_END();
}
