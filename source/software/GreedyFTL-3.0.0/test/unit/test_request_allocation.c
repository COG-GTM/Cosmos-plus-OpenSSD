/* Unit tests for request_allocation.c: the request pool and the doubly linked
 * request queues (free, slice, blocked-by-buffer-dependency,
 * blocked-by-row-address-dependency, NVMe DMA, NAND).
 *
 * The FTL is booted once so the tables reached by the release paths
 * (GetFromNandReqQ / SelectiveGetFromNvmeDmaReqQ -> ReleaseBlockedByBufDepReq)
 * are populated; each test then starts from a freshly initialised request pool
 * and works directly on it in emulated DRAM. */
#include <string.h>
#include "unity.h"
#include "ftl_test_env.h"
#include "request_allocation.h"
#include "request_transform.h"
#include "request_schedule.h"
#include "data_buffer.h"
#include "mock_io.h"
#include "nvme/host_lld.h"

#define POOL_SIZE AVAILABLE_OUNTSTANDING_REQ_COUNT
#define TEST_CH   1
#define TEST_WAY  5

/* Request pool accounting as InitFTL() left it, captured before the per-test
 * reset so the boot state itself can be asserted on. */
static struct {
	unsigned int freeReqCnt;
	unsigned int freeHead;
	unsigned int freeTail;
	unsigned int notCompletedNandReqCnt;
	unsigned int blockedReqCnt;
	unsigned int freeChainLength;
} bootState;

static unsigned int count_free_chain(void)
{
	unsigned int cur = freeReqQ.headReq, walked = 0;

	while (cur != REQ_SLOT_TAG_NONE && walked <= POOL_SIZE) {
		cur = reqPoolPtr->reqPool[cur].nextReq;
		walked++;
	}
	return walked;
}

/* Boot the FTL once (populates the data buffer tables the release paths walk),
 * then give each test a pristine request pool and host DMA state. */
void setUp(void)
{
	static int isBooted = 0;

	if (!isBooted) {
		ftl_test_env_init_ftl();
		bootState.freeReqCnt = freeReqQ.reqCnt;
		bootState.freeHead = freeReqQ.headReq;
		bootState.freeTail = freeReqQ.tailReq;
		bootState.notCompletedNandReqCnt = notCompletedNandReqCnt;
		bootState.blockedReqCnt = blockedReqCnt;
		bootState.freeChainLength = count_free_chain();
		isBooted = 1;
	}
	InitReqPool();
	InitDataBuf();
	memset(&g_hostDmaStatus, 0, sizeof(g_hostDmaStatus));
	memset(&g_hostDmaAssistStatus, 0, sizeof(g_hostDmaAssistStatus));
	mock_io_set_read_handler(HOST_DMA_FIFO_CNT_REG_ADDR, ftl_test_dma_fifo_instant_done, NULL);
}
void tearDown(void) {}

/* ------------------------------------------------------------------------ */
/* Helpers                                                                   */
/* ------------------------------------------------------------------------ */

static SSD_REQ_FORMAT *req(unsigned int tag) { return &reqPoolPtr->reqPool[tag]; }

/* Walk head -> tail following nextReq, checking prevReq back-links, the
 * per-slot queue type and the element count. */
static void assert_queue_links(unsigned int head, unsigned int tail,
                               unsigned int expectedCnt, unsigned int queueType)
{
	unsigned int cur = head, prev = REQ_SLOT_TAG_NONE, walked = 0;

	if (expectedCnt == 0) {
		TEST_ASSERT_EQUAL_UINT(REQ_SLOT_TAG_NONE, head);
		TEST_ASSERT_EQUAL_UINT(REQ_SLOT_TAG_NONE, tail);
		return;
	}

	while (cur != REQ_SLOT_TAG_NONE) {
		TEST_ASSERT_TRUE(walked < expectedCnt);
		TEST_ASSERT_EQUAL_UINT(prev, req(cur)->prevReq);
		TEST_ASSERT_EQUAL_UINT(queueType, req(cur)->reqQueueType);
		prev = cur;
		cur = req(cur)->nextReq;
		walked++;
	}
	TEST_ASSERT_EQUAL_UINT(expectedCnt, walked);
	TEST_ASSERT_EQUAL_UINT(tail, prev);
}

static void assert_free_queue_consistent(void)
{
	assert_queue_links(freeReqQ.headReq, freeReqQ.tailReq, freeReqQ.reqCnt,
	                   REQ_QUEUE_TYPE_FREE);
}

/* A request that will not trigger any dependency release when it is freed. */
static unsigned int take_detached_req(void)
{
	unsigned int tag = GetFromFreeReqQ();

	req(tag)->reqOpt.dataBufFormat = REQ_OPT_DATA_BUF_NONE;
	req(tag)->prevBlockingReq = REQ_SLOT_TAG_NONE;
	req(tag)->nextBlockingReq = REQ_SLOT_TAG_NONE;
	return tag;
}

static void drain_free_queue(void)
{
	while (freeReqQ.headReq != REQ_SLOT_TAG_NONE)
		take_detached_req();
}

/* ------------------------------------------------------------------------ */
/* InitReqPool                                                               */
/* ------------------------------------------------------------------------ */

static void test_init_links_every_slot_into_free_queue(void)
{
	unsigned int tag;

	InitReqPool();

	TEST_ASSERT_EQUAL_UINT(0, freeReqQ.headReq);
	TEST_ASSERT_EQUAL_UINT(POOL_SIZE - 1, freeReqQ.tailReq);
	TEST_ASSERT_EQUAL_UINT(POOL_SIZE, freeReqQ.reqCnt);
	assert_free_queue_consistent();

	for (tag = 0; tag < POOL_SIZE; tag++) {
		TEST_ASSERT_EQUAL_UINT(REQ_SLOT_TAG_NONE, req(tag)->prevBlockingReq);
		TEST_ASSERT_EQUAL_UINT(REQ_SLOT_TAG_NONE, req(tag)->nextBlockingReq);
	}
	TEST_ASSERT_EQUAL_UINT(REQ_SLOT_TAG_NONE, req(0)->prevReq);
	TEST_ASSERT_EQUAL_UINT(REQ_SLOT_TAG_NONE, req(POOL_SIZE - 1)->nextReq);
}

static void test_init_empties_every_other_queue_and_counters(void)
{
	unsigned int ch, way;

	/* Dirty the state first so the reset is observable. */
	PutToSliceReqQ(take_detached_req());
	PutToBlockedByBufDepReqQ(take_detached_req());
	PutToNvmeDmaReqQ(take_detached_req());
	PutToBlockedByRowAddrDepReqQ(take_detached_req(), TEST_CH, TEST_WAY);
	PutToNandReqQ(take_detached_req(), TEST_CH, TEST_WAY);

	InitReqPool();

	assert_queue_links(sliceReqQ.headReq, sliceReqQ.tailReq, 0, 0);
	TEST_ASSERT_EQUAL_UINT(0, sliceReqQ.reqCnt);
	assert_queue_links(blockedByBufDepReqQ.headReq, blockedByBufDepReqQ.tailReq, 0, 0);
	TEST_ASSERT_EQUAL_UINT(0, blockedByBufDepReqQ.reqCnt);
	assert_queue_links(nvmeDmaReqQ.headReq, nvmeDmaReqQ.tailReq, 0, 0);
	TEST_ASSERT_EQUAL_UINT(0, nvmeDmaReqQ.reqCnt);

	for (ch = 0; ch < USER_CHANNELS; ch++)
		for (way = 0; way < USER_WAYS; way++) {
			assert_queue_links(blockedByRowAddrDepReqQ[ch][way].headReq,
			                   blockedByRowAddrDepReqQ[ch][way].tailReq, 0, 0);
			TEST_ASSERT_EQUAL_UINT(0, blockedByRowAddrDepReqQ[ch][way].reqCnt);
			assert_queue_links(nandReqQ[ch][way].headReq, nandReqQ[ch][way].tailReq, 0, 0);
			TEST_ASSERT_EQUAL_UINT(0, nandReqQ[ch][way].reqCnt);
		}

	TEST_ASSERT_EQUAL_UINT(0, notCompletedNandReqCnt);
	TEST_ASSERT_EQUAL_UINT(0, blockedReqCnt);
}

static void test_ftl_boot_leaves_pool_fully_free(void)
{
	TEST_ASSERT_EQUAL_UINT(POOL_SIZE, bootState.freeReqCnt);
	TEST_ASSERT_EQUAL_UINT(POOL_SIZE, bootState.freeChainLength);
	TEST_ASSERT_NOT_EQUAL(REQ_SLOT_TAG_NONE, bootState.freeHead);
	TEST_ASSERT_NOT_EQUAL(REQ_SLOT_TAG_NONE, bootState.freeTail);
	TEST_ASSERT_EQUAL_UINT(0, bootState.notCompletedNandReqCnt);
	TEST_ASSERT_EQUAL_UINT(0, bootState.blockedReqCnt);
}

/* ------------------------------------------------------------------------ */
/* Free request queue                                                        */
/* ------------------------------------------------------------------------ */

static void test_free_queue_hands_out_slots_from_head_in_order(void)
{
	unsigned int first = freeReqQ.headReq;
	unsigned int second = req(first)->nextReq;
	unsigned int got;

	got = GetFromFreeReqQ();
	TEST_ASSERT_EQUAL_UINT(first, got);
	TEST_ASSERT_EQUAL_UINT(REQ_QUEUE_TYPE_NONE, req(got)->reqQueueType);
	TEST_ASSERT_EQUAL_UINT(second, freeReqQ.headReq);
	TEST_ASSERT_EQUAL_UINT(REQ_SLOT_TAG_NONE, req(second)->prevReq);
	TEST_ASSERT_EQUAL_UINT(POOL_SIZE - 1, freeReqQ.reqCnt);

	TEST_ASSERT_EQUAL_UINT(second, GetFromFreeReqQ());
	TEST_ASSERT_EQUAL_UINT(POOL_SIZE - 2, freeReqQ.reqCnt);
	assert_free_queue_consistent();
}

static void test_free_queue_put_appends_at_tail(void)
{
	unsigned int a = take_detached_req();
	unsigned int b = take_detached_req();
	unsigned int oldTail = freeReqQ.tailReq;

	PutToFreeReqQ(a);
	TEST_ASSERT_EQUAL_UINT(a, freeReqQ.tailReq);
	TEST_ASSERT_EQUAL_UINT(oldTail, req(a)->prevReq);
	TEST_ASSERT_EQUAL_UINT(a, req(oldTail)->nextReq);
	TEST_ASSERT_EQUAL_UINT(REQ_SLOT_TAG_NONE, req(a)->nextReq);
	TEST_ASSERT_EQUAL_UINT(REQ_QUEUE_TYPE_FREE, req(a)->reqQueueType);

	PutToFreeReqQ(b);
	TEST_ASSERT_EQUAL_UINT(b, freeReqQ.tailReq);
	TEST_ASSERT_EQUAL_UINT(a, req(b)->prevReq);
	TEST_ASSERT_EQUAL_UINT(POOL_SIZE, freeReqQ.reqCnt);
	assert_free_queue_consistent();
}

static void test_free_queue_drains_to_empty_and_refills_from_empty(void)
{
	unsigned int seen[POOL_SIZE] = {0};
	unsigned int tags[POOL_SIZE];
	unsigned int i;

	for (i = 0; i < POOL_SIZE; i++) {
		tags[i] = take_detached_req();
		TEST_ASSERT_TRUE_MESSAGE(tags[i] < POOL_SIZE, "tag out of range");
		TEST_ASSERT_FALSE_MESSAGE(seen[tags[i]], "slot handed out twice");
		seen[tags[i]] = 1;
	}
	TEST_ASSERT_EQUAL_UINT(0, freeReqQ.reqCnt);
	TEST_ASSERT_EQUAL_UINT(REQ_SLOT_TAG_NONE, freeReqQ.headReq);
	TEST_ASSERT_EQUAL_UINT(REQ_SLOT_TAG_NONE, freeReqQ.tailReq);

	/* Putting into an empty queue makes the slot both head and tail. */
	PutToFreeReqQ(tags[3]);
	TEST_ASSERT_EQUAL_UINT(tags[3], freeReqQ.headReq);
	TEST_ASSERT_EQUAL_UINT(tags[3], freeReqQ.tailReq);
	TEST_ASSERT_EQUAL_UINT(REQ_SLOT_TAG_NONE, req(tags[3])->prevReq);
	TEST_ASSERT_EQUAL_UINT(REQ_SLOT_TAG_NONE, req(tags[3])->nextReq);
	TEST_ASSERT_EQUAL_UINT(1, freeReqQ.reqCnt);

	/* Taking the single remaining slot empties the queue again. */
	TEST_ASSERT_EQUAL_UINT(tags[3], GetFromFreeReqQ());
	TEST_ASSERT_EQUAL_UINT(REQ_SLOT_TAG_NONE, freeReqQ.headReq);
	TEST_ASSERT_EQUAL_UINT(REQ_SLOT_TAG_NONE, freeReqQ.tailReq);
	TEST_ASSERT_EQUAL_UINT(0, freeReqQ.reqCnt);

	for (i = 0; i < POOL_SIZE; i++)
		PutToFreeReqQ(tags[i]);
	TEST_ASSERT_EQUAL_UINT(POOL_SIZE, freeReqQ.reqCnt);
	assert_free_queue_consistent();
}

/* Host DMA FIFO count register model for an in-flight TxDMA: the hardware
 * head stays behind the software tail for the first `pendingPolls` reads,
 * then catches up (DMA complete). */
struct dma_fifo_model {
	unsigned int pendingPolls;
	unsigned int polls;
};

static uint32_t dma_fifo_pending_then_done(uintptr_t addr, uint32_t stored, void *ctx)
{
	struct dma_fifo_model *model = ctx;
	HOST_DMA_FIFO_CNT_REG head;

	(void)addr; (void)stored;
	model->polls++;
	if (model->polls <= model->pendingPolls) {
		head.dword = g_hostDmaStatus.fifoTail.dword;
		head.autoDmaTx = 0;
		return head.dword;
	}
	return g_hostDmaStatus.fifoTail.dword;
}

/* Exhaustion: with the pool empty, GetFromFreeReqQ() must spin in
 * SyncAvailFreeReq() until an outstanding request completes. A TxDMA request
 * is issued (software FIFO tail advanced) and parked on the NVMe DMA queue;
 * the FIFO model keeps it pending for several polls, so the spin must iterate
 * and then hand back exactly that slot. */
static void test_free_queue_exhaustion_spins_until_inflight_dma_completes(void)
{
	struct dma_fifo_model model = { .pendingPolls = 3, .polls = 0 };
	unsigned int dmaTag, got;

	drain_free_queue();
	TEST_ASSERT_EQUAL_UINT(REQ_SLOT_TAG_NONE, freeReqQ.headReq);

	g_hostDmaStatus.fifoTail.autoDmaTx = 1;
	mock_io_set_read_handler(HOST_DMA_FIFO_CNT_REG_ADDR, dma_fifo_pending_then_done, &model);

	dmaTag = 7;
	req(dmaTag)->reqType = REQ_TYPE_NVME_DMA;
	req(dmaTag)->reqCode = REQ_CODE_TxDMA;
	req(dmaTag)->nvmeDmaInfo.reqTail = g_hostDmaStatus.fifoTail.autoDmaTx;
	req(dmaTag)->nvmeDmaInfo.overFlowCnt = g_hostDmaAssistStatus.autoDmaTxOverFlowCnt;
	PutToNvmeDmaReqQ(dmaTag);
	TEST_ASSERT_EQUAL_UINT(1, nvmeDmaReqQ.reqCnt);

	got = GetFromFreeReqQ();

	TEST_ASSERT_EQUAL_UINT(dmaTag, got);
	TEST_ASSERT_EQUAL_UINT(model.pendingPolls + 1, model.polls);
	TEST_ASSERT_EQUAL_UINT(0, nvmeDmaReqQ.reqCnt);
	TEST_ASSERT_EQUAL_UINT(REQ_SLOT_TAG_NONE, nvmeDmaReqQ.headReq);
	TEST_ASSERT_EQUAL_UINT(0, freeReqQ.reqCnt);
	TEST_ASSERT_EQUAL_UINT(REQ_QUEUE_TYPE_NONE, req(got)->reqQueueType);
}

/* ------------------------------------------------------------------------ */
/* Slice request queue                                                       */
/* ------------------------------------------------------------------------ */

static void test_slice_queue_get_on_empty_returns_fail(void)
{
	TEST_ASSERT_EQUAL_UINT(REQ_SLOT_TAG_FAIL, GetFromSliceReqQ());
	TEST_ASSERT_EQUAL_UINT(0, sliceReqQ.reqCnt);
}

static void test_slice_queue_is_fifo_and_tracks_links(void)
{
	unsigned int a = take_detached_req();
	unsigned int b = take_detached_req();
	unsigned int c = take_detached_req();

	PutToSliceReqQ(a);
	TEST_ASSERT_EQUAL_UINT(a, sliceReqQ.headReq);
	TEST_ASSERT_EQUAL_UINT(a, sliceReqQ.tailReq);
	PutToSliceReqQ(b);
	PutToSliceReqQ(c);
	TEST_ASSERT_EQUAL_UINT(3, sliceReqQ.reqCnt);
	assert_queue_links(sliceReqQ.headReq, sliceReqQ.tailReq, 3, REQ_QUEUE_TYPE_SLICE);

	TEST_ASSERT_EQUAL_UINT(a, GetFromSliceReqQ());
	TEST_ASSERT_EQUAL_UINT(REQ_QUEUE_TYPE_NONE, req(a)->reqQueueType);
	TEST_ASSERT_EQUAL_UINT(b, sliceReqQ.headReq);
	TEST_ASSERT_EQUAL_UINT(REQ_SLOT_TAG_NONE, req(b)->prevReq);
	assert_queue_links(sliceReqQ.headReq, sliceReqQ.tailReq, 2, REQ_QUEUE_TYPE_SLICE);

	TEST_ASSERT_EQUAL_UINT(b, GetFromSliceReqQ());
	TEST_ASSERT_EQUAL_UINT(c, GetFromSliceReqQ());
	TEST_ASSERT_EQUAL_UINT(0, sliceReqQ.reqCnt);
	TEST_ASSERT_EQUAL_UINT(REQ_SLOT_TAG_NONE, sliceReqQ.headReq);
	TEST_ASSERT_EQUAL_UINT(REQ_SLOT_TAG_NONE, sliceReqQ.tailReq);
	TEST_ASSERT_EQUAL_UINT(REQ_SLOT_TAG_FAIL, GetFromSliceReqQ());
}

/* ------------------------------------------------------------------------ */
/* Blocked-by-buffer-dependency queue                                        */
/* ------------------------------------------------------------------------ */

static void test_buf_dep_queue_put_counts_blocked_requests(void)
{
	unsigned int a = take_detached_req();
	unsigned int b = take_detached_req();

	PutToBlockedByBufDepReqQ(a);
	TEST_ASSERT_EQUAL_UINT(a, blockedByBufDepReqQ.headReq);
	TEST_ASSERT_EQUAL_UINT(a, blockedByBufDepReqQ.tailReq);
	TEST_ASSERT_EQUAL_UINT(1, blockedByBufDepReqQ.reqCnt);
	TEST_ASSERT_EQUAL_UINT(1, blockedReqCnt);
	TEST_ASSERT_EQUAL_UINT(REQ_QUEUE_TYPE_BLOCKED_BY_BUF_DEP, req(a)->reqQueueType);

	PutToBlockedByBufDepReqQ(b);
	TEST_ASSERT_EQUAL_UINT(b, blockedByBufDepReqQ.tailReq);
	TEST_ASSERT_EQUAL_UINT(a, req(b)->prevReq);
	TEST_ASSERT_EQUAL_UINT(b, req(a)->nextReq);
	TEST_ASSERT_EQUAL_UINT(2, blockedReqCnt);
	assert_queue_links(blockedByBufDepReqQ.headReq, blockedByBufDepReqQ.tailReq, 2,
	                   REQ_QUEUE_TYPE_BLOCKED_BY_BUF_DEP);
}

static void test_buf_dep_queue_selective_remove_middle(void)
{
	unsigned int a = take_detached_req(), b = take_detached_req(), c = take_detached_req();

	PutToBlockedByBufDepReqQ(a);
	PutToBlockedByBufDepReqQ(b);
	PutToBlockedByBufDepReqQ(c);

	SelectiveGetFromBlockedByBufDepReqQ(b);

	TEST_ASSERT_EQUAL_UINT(a, blockedByBufDepReqQ.headReq);
	TEST_ASSERT_EQUAL_UINT(c, blockedByBufDepReqQ.tailReq);
	TEST_ASSERT_EQUAL_UINT(c, req(a)->nextReq);
	TEST_ASSERT_EQUAL_UINT(a, req(c)->prevReq);
	TEST_ASSERT_EQUAL_UINT(REQ_QUEUE_TYPE_NONE, req(b)->reqQueueType);
	TEST_ASSERT_EQUAL_UINT(2, blockedByBufDepReqQ.reqCnt);
	TEST_ASSERT_EQUAL_UINT(2, blockedReqCnt);
	assert_queue_links(blockedByBufDepReqQ.headReq, blockedByBufDepReqQ.tailReq, 2,
	                   REQ_QUEUE_TYPE_BLOCKED_BY_BUF_DEP);
}

static void test_buf_dep_queue_selective_remove_head(void)
{
	unsigned int a = take_detached_req(), b = take_detached_req(), c = take_detached_req();

	PutToBlockedByBufDepReqQ(a);
	PutToBlockedByBufDepReqQ(b);
	PutToBlockedByBufDepReqQ(c);

	SelectiveGetFromBlockedByBufDepReqQ(a);

	TEST_ASSERT_EQUAL_UINT(b, blockedByBufDepReqQ.headReq);
	TEST_ASSERT_EQUAL_UINT(REQ_SLOT_TAG_NONE, req(b)->prevReq);
	TEST_ASSERT_EQUAL_UINT(c, blockedByBufDepReqQ.tailReq);
	TEST_ASSERT_EQUAL_UINT(2, blockedReqCnt);
	assert_queue_links(blockedByBufDepReqQ.headReq, blockedByBufDepReqQ.tailReq, 2,
	                   REQ_QUEUE_TYPE_BLOCKED_BY_BUF_DEP);
}

static void test_buf_dep_queue_selective_remove_tail(void)
{
	unsigned int a = take_detached_req(), b = take_detached_req(), c = take_detached_req();

	PutToBlockedByBufDepReqQ(a);
	PutToBlockedByBufDepReqQ(b);
	PutToBlockedByBufDepReqQ(c);

	SelectiveGetFromBlockedByBufDepReqQ(c);

	TEST_ASSERT_EQUAL_UINT(a, blockedByBufDepReqQ.headReq);
	TEST_ASSERT_EQUAL_UINT(b, blockedByBufDepReqQ.tailReq);
	TEST_ASSERT_EQUAL_UINT(REQ_SLOT_TAG_NONE, req(b)->nextReq);
	TEST_ASSERT_EQUAL_UINT(2, blockedReqCnt);
	assert_queue_links(blockedByBufDepReqQ.headReq, blockedByBufDepReqQ.tailReq, 2,
	                   REQ_QUEUE_TYPE_BLOCKED_BY_BUF_DEP);
}

static void test_buf_dep_queue_selective_remove_only_element_empties_queue(void)
{
	unsigned int a = take_detached_req();

	PutToBlockedByBufDepReqQ(a);
	SelectiveGetFromBlockedByBufDepReqQ(a);

	TEST_ASSERT_EQUAL_UINT(REQ_SLOT_TAG_NONE, blockedByBufDepReqQ.headReq);
	TEST_ASSERT_EQUAL_UINT(REQ_SLOT_TAG_NONE, blockedByBufDepReqQ.tailReq);
	TEST_ASSERT_EQUAL_UINT(0, blockedByBufDepReqQ.reqCnt);
	TEST_ASSERT_EQUAL_UINT(0, blockedReqCnt);
}

static void test_buf_dep_queue_selective_remove_none_tag_asserts(void)
{
	FTL_TEST_EXPECT_ASSERT(SelectiveGetFromBlockedByBufDepReqQ(REQ_SLOT_TAG_NONE));
}

/* ------------------------------------------------------------------------ */
/* Blocked-by-row-address-dependency queues (per channel/way)                */
/* ------------------------------------------------------------------------ */

static void test_row_addr_dep_queues_are_independent_per_die(void)
{
	unsigned int a = take_detached_req(), b = take_detached_req();

	PutToBlockedByRowAddrDepReqQ(a, 0, 0);
	PutToBlockedByRowAddrDepReqQ(b, TEST_CH, TEST_WAY);

	TEST_ASSERT_EQUAL_UINT(a, blockedByRowAddrDepReqQ[0][0].headReq);
	TEST_ASSERT_EQUAL_UINT(a, blockedByRowAddrDepReqQ[0][0].tailReq);
	TEST_ASSERT_EQUAL_UINT(1, blockedByRowAddrDepReqQ[0][0].reqCnt);
	TEST_ASSERT_EQUAL_UINT(b, blockedByRowAddrDepReqQ[TEST_CH][TEST_WAY].headReq);
	TEST_ASSERT_EQUAL_UINT(1, blockedByRowAddrDepReqQ[TEST_CH][TEST_WAY].reqCnt);
	TEST_ASSERT_EQUAL_UINT(REQ_SLOT_TAG_NONE, blockedByRowAddrDepReqQ[0][1].headReq);
	TEST_ASSERT_EQUAL_UINT(REQ_QUEUE_TYPE_BLOCKED_BY_ROW_ADDR_DEP, req(a)->reqQueueType);
	TEST_ASSERT_EQUAL_UINT(2, blockedReqCnt);
}

static void test_row_addr_dep_queue_selective_remove_middle_head_tail(void)
{
	unsigned int a = take_detached_req(), b = take_detached_req();
	unsigned int c = take_detached_req(), d = take_detached_req();
	BLOCKED_BY_ROW_ADDR_DEPENDENCY_REQUEST_QUEUE *q = &blockedByRowAddrDepReqQ[TEST_CH][TEST_WAY];

	PutToBlockedByRowAddrDepReqQ(a, TEST_CH, TEST_WAY);
	PutToBlockedByRowAddrDepReqQ(b, TEST_CH, TEST_WAY);
	PutToBlockedByRowAddrDepReqQ(c, TEST_CH, TEST_WAY);
	PutToBlockedByRowAddrDepReqQ(d, TEST_CH, TEST_WAY);
	TEST_ASSERT_EQUAL_UINT(4, q->reqCnt);
	TEST_ASSERT_EQUAL_UINT(4, blockedReqCnt);
	assert_queue_links(q->headReq, q->tailReq, 4, REQ_QUEUE_TYPE_BLOCKED_BY_ROW_ADDR_DEP);

	/* middle */
	SelectiveGetFromBlockedByRowAddrDepReqQ(b, TEST_CH, TEST_WAY);
	TEST_ASSERT_EQUAL_UINT(c, req(a)->nextReq);
	TEST_ASSERT_EQUAL_UINT(a, req(c)->prevReq);
	TEST_ASSERT_EQUAL_UINT(REQ_QUEUE_TYPE_NONE, req(b)->reqQueueType);
	assert_queue_links(q->headReq, q->tailReq, 3, REQ_QUEUE_TYPE_BLOCKED_BY_ROW_ADDR_DEP);

	/* head */
	SelectiveGetFromBlockedByRowAddrDepReqQ(a, TEST_CH, TEST_WAY);
	TEST_ASSERT_EQUAL_UINT(c, q->headReq);
	TEST_ASSERT_EQUAL_UINT(REQ_SLOT_TAG_NONE, req(c)->prevReq);
	assert_queue_links(q->headReq, q->tailReq, 2, REQ_QUEUE_TYPE_BLOCKED_BY_ROW_ADDR_DEP);

	/* tail */
	SelectiveGetFromBlockedByRowAddrDepReqQ(d, TEST_CH, TEST_WAY);
	TEST_ASSERT_EQUAL_UINT(c, q->tailReq);
	TEST_ASSERT_EQUAL_UINT(REQ_SLOT_TAG_NONE, req(c)->nextReq);
	assert_queue_links(q->headReq, q->tailReq, 1, REQ_QUEUE_TYPE_BLOCKED_BY_ROW_ADDR_DEP);

	/* last */
	SelectiveGetFromBlockedByRowAddrDepReqQ(c, TEST_CH, TEST_WAY);
	TEST_ASSERT_EQUAL_UINT(REQ_SLOT_TAG_NONE, q->headReq);
	TEST_ASSERT_EQUAL_UINT(REQ_SLOT_TAG_NONE, q->tailReq);
	TEST_ASSERT_EQUAL_UINT(0, q->reqCnt);
	TEST_ASSERT_EQUAL_UINT(0, blockedReqCnt);
}

static void test_row_addr_dep_queue_selective_remove_none_tag_asserts(void)
{
	FTL_TEST_EXPECT_ASSERT(SelectiveGetFromBlockedByRowAddrDepReqQ(REQ_SLOT_TAG_NONE, 0, 0));
}

/* ------------------------------------------------------------------------ */
/* NVMe DMA queue                                                            */
/* ------------------------------------------------------------------------ */

static void test_nvme_dma_queue_put_links_at_tail(void)
{
	unsigned int a = take_detached_req(), b = take_detached_req();

	PutToNvmeDmaReqQ(a);
	TEST_ASSERT_EQUAL_UINT(a, nvmeDmaReqQ.headReq);
	TEST_ASSERT_EQUAL_UINT(a, nvmeDmaReqQ.tailReq);
	TEST_ASSERT_EQUAL_UINT(REQ_QUEUE_TYPE_NVME_DMA, req(a)->reqQueueType);

	PutToNvmeDmaReqQ(b);
	TEST_ASSERT_EQUAL_UINT(b, nvmeDmaReqQ.tailReq);
	TEST_ASSERT_EQUAL_UINT(a, req(b)->prevReq);
	TEST_ASSERT_EQUAL_UINT(b, req(a)->nextReq);
	TEST_ASSERT_EQUAL_UINT(2, nvmeDmaReqQ.reqCnt);
	assert_queue_links(nvmeDmaReqQ.headReq, nvmeDmaReqQ.tailReq, 2, REQ_QUEUE_TYPE_NVME_DMA);
}

static void test_nvme_dma_queue_selective_remove_returns_slot_to_free_queue(void)
{
	unsigned int a = take_detached_req(), b = take_detached_req(), c = take_detached_req();
	unsigned int freeBefore = freeReqQ.reqCnt;

	PutToNvmeDmaReqQ(a);
	PutToNvmeDmaReqQ(b);
	PutToNvmeDmaReqQ(c);

	/* middle */
	SelectiveGetFromNvmeDmaReqQ(b);
	TEST_ASSERT_EQUAL_UINT(2, nvmeDmaReqQ.reqCnt);
	TEST_ASSERT_EQUAL_UINT(c, req(a)->nextReq);
	TEST_ASSERT_EQUAL_UINT(a, req(c)->prevReq);
	TEST_ASSERT_EQUAL_UINT(REQ_QUEUE_TYPE_FREE, req(b)->reqQueueType);
	TEST_ASSERT_EQUAL_UINT(b, freeReqQ.tailReq);
	TEST_ASSERT_EQUAL_UINT(freeBefore + 1, freeReqQ.reqCnt);
	assert_queue_links(nvmeDmaReqQ.headReq, nvmeDmaReqQ.tailReq, 2, REQ_QUEUE_TYPE_NVME_DMA);

	/* tail */
	SelectiveGetFromNvmeDmaReqQ(c);
	TEST_ASSERT_EQUAL_UINT(a, nvmeDmaReqQ.tailReq);
	TEST_ASSERT_EQUAL_UINT(REQ_SLOT_TAG_NONE, req(a)->nextReq);
	TEST_ASSERT_EQUAL_UINT(c, freeReqQ.tailReq);

	/* head == only */
	SelectiveGetFromNvmeDmaReqQ(a);
	TEST_ASSERT_EQUAL_UINT(REQ_SLOT_TAG_NONE, nvmeDmaReqQ.headReq);
	TEST_ASSERT_EQUAL_UINT(REQ_SLOT_TAG_NONE, nvmeDmaReqQ.tailReq);
	TEST_ASSERT_EQUAL_UINT(0, nvmeDmaReqQ.reqCnt);
	TEST_ASSERT_EQUAL_UINT(freeBefore + 3, freeReqQ.reqCnt);
	assert_free_queue_consistent();
}

static void test_nvme_dma_queue_selective_remove_head_with_successor(void)
{
	unsigned int a = take_detached_req(), b = take_detached_req();

	PutToNvmeDmaReqQ(a);
	PutToNvmeDmaReqQ(b);

	SelectiveGetFromNvmeDmaReqQ(a);

	TEST_ASSERT_EQUAL_UINT(b, nvmeDmaReqQ.headReq);
	TEST_ASSERT_EQUAL_UINT(b, nvmeDmaReqQ.tailReq);
	TEST_ASSERT_EQUAL_UINT(REQ_SLOT_TAG_NONE, req(b)->prevReq);
	TEST_ASSERT_EQUAL_UINT(1, nvmeDmaReqQ.reqCnt);
	TEST_ASSERT_EQUAL_UINT(REQ_QUEUE_TYPE_FREE, req(a)->reqQueueType);
}

/* ------------------------------------------------------------------------ */
/* NAND queues (per channel/way)                                             */
/* ------------------------------------------------------------------------ */

static void test_nand_queue_put_tracks_not_completed_count(void)
{
	unsigned int a = take_detached_req(), b = take_detached_req(), c = take_detached_req();

	PutToNandReqQ(a, TEST_CH, TEST_WAY);
	TEST_ASSERT_EQUAL_UINT(a, nandReqQ[TEST_CH][TEST_WAY].headReq);
	TEST_ASSERT_EQUAL_UINT(a, nandReqQ[TEST_CH][TEST_WAY].tailReq);
	TEST_ASSERT_EQUAL_UINT(REQ_QUEUE_TYPE_NAND, req(a)->reqQueueType);
	TEST_ASSERT_EQUAL_UINT(1, notCompletedNandReqCnt);

	PutToNandReqQ(b, TEST_CH, TEST_WAY);
	PutToNandReqQ(c, 0, 0);
	TEST_ASSERT_EQUAL_UINT(b, nandReqQ[TEST_CH][TEST_WAY].tailReq);
	TEST_ASSERT_EQUAL_UINT(a, req(b)->prevReq);
	TEST_ASSERT_EQUAL_UINT(2, nandReqQ[TEST_CH][TEST_WAY].reqCnt);
	TEST_ASSERT_EQUAL_UINT(1, nandReqQ[0][0].reqCnt);
	TEST_ASSERT_EQUAL_UINT(3, notCompletedNandReqCnt);
	assert_queue_links(nandReqQ[TEST_CH][TEST_WAY].headReq, nandReqQ[TEST_CH][TEST_WAY].tailReq,
	                   2, REQ_QUEUE_TYPE_NAND);
}

static void test_nand_queue_get_is_fifo_and_frees_slot(void)
{
	unsigned int a = take_detached_req(), b = take_detached_req();
	unsigned int freeBefore = freeReqQ.reqCnt;

	PutToNandReqQ(a, TEST_CH, TEST_WAY);
	PutToNandReqQ(b, TEST_CH, TEST_WAY);

	GetFromNandReqQ(TEST_CH, TEST_WAY, 0, REQ_CODE_READ);
	TEST_ASSERT_EQUAL_UINT(b, nandReqQ[TEST_CH][TEST_WAY].headReq);
	TEST_ASSERT_EQUAL_UINT(REQ_SLOT_TAG_NONE, req(b)->prevReq);
	TEST_ASSERT_EQUAL_UINT(1, nandReqQ[TEST_CH][TEST_WAY].reqCnt);
	TEST_ASSERT_EQUAL_UINT(1, notCompletedNandReqCnt);
	TEST_ASSERT_EQUAL_UINT(REQ_QUEUE_TYPE_FREE, req(a)->reqQueueType);
	TEST_ASSERT_EQUAL_UINT(a, freeReqQ.tailReq);
	TEST_ASSERT_EQUAL_UINT(freeBefore + 1, freeReqQ.reqCnt);

	GetFromNandReqQ(TEST_CH, TEST_WAY, 0, REQ_CODE_WRITE);
	TEST_ASSERT_EQUAL_UINT(REQ_SLOT_TAG_NONE, nandReqQ[TEST_CH][TEST_WAY].headReq);
	TEST_ASSERT_EQUAL_UINT(REQ_SLOT_TAG_NONE, nandReqQ[TEST_CH][TEST_WAY].tailReq);
	TEST_ASSERT_EQUAL_UINT(0, nandReqQ[TEST_CH][TEST_WAY].reqCnt);
	TEST_ASSERT_EQUAL_UINT(0, notCompletedNandReqCnt);
	TEST_ASSERT_EQUAL_UINT(b, freeReqQ.tailReq);
	TEST_ASSERT_EQUAL_UINT(freeBefore + 2, freeReqQ.reqCnt);
	assert_free_queue_consistent();
}

static void test_nand_queue_get_on_empty_queue_asserts(void)
{
	FTL_TEST_EXPECT_ASSERT(GetFromNandReqQ(TEST_CH, TEST_WAY, 0, REQ_CODE_READ));
}

/* Completing a NAND request must wake the request chained behind it through
 * nextBlockingReq: the waiter leaves the buffer-dependency queue and, being
 * an NVMe DMA request, is issued and queued on nvmeDmaReqQ. */
static void test_nand_completion_releases_buffer_blocked_dma_request(void)
{
	unsigned int nandTag = take_detached_req();
	unsigned int waiterTag = take_detached_req();
	unsigned int bufEntry = 3;

	req(nandTag)->reqType = REQ_TYPE_NAND;
	req(nandTag)->reqOpt.dataBufFormat = REQ_OPT_DATA_BUF_ENTRY;
	req(nandTag)->dataBufInfo.entry = bufEntry;
	req(nandTag)->nextBlockingReq = waiterTag;
	dataBufMapPtr->dataBuf[bufEntry].blockingReqTail = waiterTag;

	req(waiterTag)->reqType = REQ_TYPE_NVME_DMA;
	req(waiterTag)->reqCode = REQ_CODE_TxDMA;
	req(waiterTag)->nvmeCmdSlotTag = 0;
	req(waiterTag)->reqOpt.dataBufFormat = REQ_OPT_DATA_BUF_ENTRY;
	req(waiterTag)->dataBufInfo.entry = bufEntry;
	req(waiterTag)->nvmeDmaInfo.startIndex = 0;
	req(waiterTag)->nvmeDmaInfo.nvmeBlockOffset = 0;
	req(waiterTag)->nvmeDmaInfo.numOfNvmeBlock = 1;
	req(waiterTag)->prevBlockingReq = nandTag;

	PutToNandReqQ(nandTag, TEST_CH, TEST_WAY);
	PutToBlockedByBufDepReqQ(waiterTag);
	TEST_ASSERT_EQUAL_UINT(1, blockedReqCnt);

	GetFromNandReqQ(TEST_CH, TEST_WAY, 0, REQ_CODE_READ);

	TEST_ASSERT_EQUAL_UINT(0, notCompletedNandReqCnt);
	TEST_ASSERT_EQUAL_UINT(0, blockedReqCnt);
	TEST_ASSERT_EQUAL_UINT(0, blockedByBufDepReqQ.reqCnt);
	TEST_ASSERT_EQUAL_UINT(REQ_SLOT_TAG_NONE, req(nandTag)->nextBlockingReq);
	TEST_ASSERT_EQUAL_UINT(REQ_SLOT_TAG_NONE, req(waiterTag)->prevBlockingReq);
	TEST_ASSERT_EQUAL_UINT(REQ_QUEUE_TYPE_NVME_DMA, req(waiterTag)->reqQueueType);
	TEST_ASSERT_EQUAL_UINT(waiterTag, nvmeDmaReqQ.headReq);
	TEST_ASSERT_EQUAL_UINT(1, nvmeDmaReqQ.reqCnt);
	TEST_ASSERT_EQUAL_UINT(REQ_QUEUE_TYPE_FREE, req(nandTag)->reqQueueType);
}

/* ------------------------------------------------------------------------ */
/* Cross-queue accounting                                                    */
/* ------------------------------------------------------------------------ */

static void test_slots_are_conserved_across_all_queues(void)
{
	unsigned int tags[6], i, ch, way, inQueues = 0;

	for (i = 0; i < 6; i++)
		tags[i] = take_detached_req();

	PutToSliceReqQ(tags[0]);
	PutToBlockedByBufDepReqQ(tags[1]);
	PutToBlockedByRowAddrDepReqQ(tags[2], 0, 3);
	PutToNvmeDmaReqQ(tags[3]);
	PutToNandReqQ(tags[4], 1, 7);
	/* tags[5] is held by the "caller" and belongs to no queue. */

	inQueues += sliceReqQ.reqCnt + blockedByBufDepReqQ.reqCnt + nvmeDmaReqQ.reqCnt;
	for (ch = 0; ch < USER_CHANNELS; ch++)
		for (way = 0; way < USER_WAYS; way++)
			inQueues += blockedByRowAddrDepReqQ[ch][way].reqCnt + nandReqQ[ch][way].reqCnt;

	TEST_ASSERT_EQUAL_UINT(5, inQueues);
	TEST_ASSERT_EQUAL_UINT(POOL_SIZE - 6, freeReqQ.reqCnt);
	TEST_ASSERT_EQUAL_UINT(2, blockedReqCnt);
	TEST_ASSERT_EQUAL_UINT(1, notCompletedNandReqCnt);

	/* Return everything and make sure the free list is whole again. */
	PutToFreeReqQ(GetFromSliceReqQ());
	SelectiveGetFromBlockedByBufDepReqQ(tags[1]);
	PutToFreeReqQ(tags[1]);
	SelectiveGetFromBlockedByRowAddrDepReqQ(tags[2], 0, 3);
	PutToFreeReqQ(tags[2]);
	SelectiveGetFromNvmeDmaReqQ(tags[3]);
	GetFromNandReqQ(1, 7, 0, REQ_CODE_READ);
	PutToFreeReqQ(tags[5]);

	TEST_ASSERT_EQUAL_UINT(POOL_SIZE, freeReqQ.reqCnt);
	TEST_ASSERT_EQUAL_UINT(0, blockedReqCnt);
	TEST_ASSERT_EQUAL_UINT(0, notCompletedNandReqCnt);
	assert_free_queue_consistent();
}

int main(void)
{
	UNITY_BEGIN();
	RUN_TEST(test_init_links_every_slot_into_free_queue);
	RUN_TEST(test_init_empties_every_other_queue_and_counters);
	RUN_TEST(test_ftl_boot_leaves_pool_fully_free);
	RUN_TEST(test_free_queue_hands_out_slots_from_head_in_order);
	RUN_TEST(test_free_queue_put_appends_at_tail);
	RUN_TEST(test_free_queue_drains_to_empty_and_refills_from_empty);
	RUN_TEST(test_free_queue_exhaustion_spins_until_inflight_dma_completes);
	RUN_TEST(test_slice_queue_get_on_empty_returns_fail);
	RUN_TEST(test_slice_queue_is_fifo_and_tracks_links);
	RUN_TEST(test_buf_dep_queue_put_counts_blocked_requests);
	RUN_TEST(test_buf_dep_queue_selective_remove_middle);
	RUN_TEST(test_buf_dep_queue_selective_remove_head);
	RUN_TEST(test_buf_dep_queue_selective_remove_tail);
	RUN_TEST(test_buf_dep_queue_selective_remove_only_element_empties_queue);
	RUN_TEST(test_buf_dep_queue_selective_remove_none_tag_asserts);
	RUN_TEST(test_row_addr_dep_queues_are_independent_per_die);
	RUN_TEST(test_row_addr_dep_queue_selective_remove_middle_head_tail);
	RUN_TEST(test_row_addr_dep_queue_selective_remove_none_tag_asserts);
	RUN_TEST(test_nvme_dma_queue_put_links_at_tail);
	RUN_TEST(test_nvme_dma_queue_selective_remove_returns_slot_to_free_queue);
	RUN_TEST(test_nvme_dma_queue_selective_remove_head_with_successor);
	RUN_TEST(test_nand_queue_put_tracks_not_completed_count);
	RUN_TEST(test_nand_queue_get_is_fifo_and_frees_slot);
	RUN_TEST(test_nand_queue_get_on_empty_queue_asserts);
	RUN_TEST(test_nand_completion_releases_buffer_blocked_dma_request);
	RUN_TEST(test_slots_are_conserved_across_all_queues);
	return UNITY_END();
}
