/* Unit tests for request_schedule.c way-priority list helpers, InitReqScheduler
 * and the NAND row / data buffer address generators. */
#include "unity.h"

#include "fw_test.h"
#include "memory_map.h"

/* request_schedule.h declares this as dieStatusTablePtr (typo); the
 * definition in request_schedule.c is dieStateTablePtr. */
extern P_DIE_STATE_TABLE dieStateTablePtr;

#define TEST_CH 0

typedef void (*list_op_t)(unsigned int chNo, unsigned int wayNo);

typedef struct {
	const char *name;
	list_op_t put;
	list_op_t get;
	/* offsets into the WAY_PRIORITY_ENTRY, read via accessor below */
	unsigned int (*head)(unsigned int chNo);
	unsigned int (*tail)(unsigned int chNo);
	void (*clear)(unsigned int chNo);
} list_desc_t;

#define P(ch) (wayPriorityTablePtr->wayPriority[ch])

#define DEFINE_LIST_ACCESSORS(prefix, headField, tailField)                       \
	static unsigned int prefix##_head(unsigned int ch) { return P(ch).headField; } \
	static unsigned int prefix##_tail(unsigned int ch) { return P(ch).tailField; } \
	static void prefix##_clear(unsigned int ch)                                    \
	{                                                                              \
		P(ch).headField = WAY_NONE;                                                \
		P(ch).tailField = WAY_NONE;                                                \
	}

DEFINE_LIST_ACCESSORS(idle, idleHead, idleTail)
DEFINE_LIST_ACCESSORS(status_report, statusReportHead, statusReportTail)
DEFINE_LIST_ACCESSORS(read_trigger, readTriggerHead, readTriggerTail)
DEFINE_LIST_ACCESSORS(write, writeHead, writeTail)
DEFINE_LIST_ACCESSORS(read_transfer, readTransferHead, readTransferTail)
DEFINE_LIST_ACCESSORS(erase, eraseHead, eraseTail)
DEFINE_LIST_ACCESSORS(status_check, statusCheckHead, statusCheckTail)

static const list_desc_t lists[] = {
	{ "idle", PutToNandIdleList, SelectivGetFromNandIdleList, idle_head, idle_tail, idle_clear },
	{ "statusReport", PutToNandStatusReportList, SelectivGetFromNandStatusReportList,
	  status_report_head, status_report_tail, status_report_clear },
	{ "readTrigger", PutToNandReadTriggerList, SelectiveGetFromNandReadTriggerList,
	  read_trigger_head, read_trigger_tail, read_trigger_clear },
	{ "write", PutToNandWriteList, SelectiveGetFromNandWriteList, write_head, write_tail, write_clear },
	{ "readTransfer", PutToNandReadTransferList, SelectiveGetFromNandReadTransferList,
	  read_transfer_head, read_transfer_tail, read_transfer_clear },
	{ "erase", PutToNandEraseList, SelectiveGetFromNandEraseList, erase_head, erase_tail, erase_clear },
	{ "statusCheck", PutToNandStatusCheckList, SelectiveGetFromNandStatusCheckList,
	  status_check_head, status_check_tail, status_check_clear },
};

#define LIST_COUNT (sizeof(lists) / sizeof(lists[0]))

void setUp(void)
{
	fw_test_reset();
	fw_test_init_ftl();
	mock_nsc_reset();
}

void tearDown(void) {}

static SSD_REQ_FORMAT *req(unsigned int reqSlotTag)
{
	return &reqPoolPtr->reqPool[reqSlotTag];
}

/* Walk head -> tail via nextWay, return the ways in order; also verify the
 * prevWay back-links and the tail pointer. */
static unsigned int walk_list(const list_desc_t *l, unsigned int chNo, unsigned int out[USER_WAYS])
{
	unsigned int count = 0;
	unsigned int prev = WAY_NONE;
	unsigned int wayNo = l->head(chNo);

	while (wayNo != WAY_NONE) {
		TEST_ASSERT_TRUE_MESSAGE(count < USER_WAYS, l->name);
		TEST_ASSERT_EQUAL_UINT_MESSAGE(prev, dieStateTablePtr->dieState[chNo][wayNo].prevWay, l->name);
		out[count++] = wayNo;
		prev = wayNo;
		wayNo = dieStateTablePtr->dieState[chNo][wayNo].nextWay;
	}
	if (count)
		TEST_ASSERT_EQUAL_UINT_MESSAGE(prev, l->tail(chNo), l->name);
	else
		TEST_ASSERT_EQUAL_UINT_MESSAGE(WAY_NONE, l->tail(chNo), l->name);
	return count;
}

/* ------------------------------------------------------------------------ */
/* InitReqScheduler                                                          */
/* ------------------------------------------------------------------------ */

static void test_init_puts_every_way_of_every_channel_in_idle_list(void)
{
	unsigned int chNo, wayNo, count;
	unsigned int ways[USER_WAYS];

	InitReqScheduler();

	for (chNo = 0; chNo < USER_CHANNELS; chNo++) {
		count = walk_list(&lists[0], chNo, ways);
		TEST_ASSERT_EQUAL_UINT(USER_WAYS, count);
		for (wayNo = 0; wayNo < USER_WAYS; wayNo++) {
			TEST_ASSERT_EQUAL_UINT(wayNo, ways[wayNo]);
			TEST_ASSERT_EQUAL_UINT(DIE_STATE_IDLE, dieStateTablePtr->dieState[chNo][wayNo].dieState);
			TEST_ASSERT_EQUAL_UINT(REQ_STATUS_CHECK_OPT_NONE, dieStateTablePtr->dieState[chNo][wayNo].reqStatusCheckOpt);
			TEST_ASSERT_EQUAL_INT(RETRY_LIMIT, retryLimitTablePtr->retryLimit[chNo][wayNo]);
			TEST_ASSERT_EQUAL_UINT(0, completeFlagTablePtr->completeFlag[chNo][wayNo]);
			TEST_ASSERT_EQUAL_UINT(0, statusReportTablePtr->statusReport[chNo][wayNo]);
		}
		TEST_ASSERT_EQUAL_UINT(WAY_NONE, P(chNo).statusReportHead);
		TEST_ASSERT_EQUAL_UINT(WAY_NONE, P(chNo).readTriggerHead);
		TEST_ASSERT_EQUAL_UINT(WAY_NONE, P(chNo).writeHead);
		TEST_ASSERT_EQUAL_UINT(WAY_NONE, P(chNo).readTransferHead);
		TEST_ASSERT_EQUAL_UINT(WAY_NONE, P(chNo).eraseHead);
		TEST_ASSERT_EQUAL_UINT(WAY_NONE, P(chNo).statusCheckHead);
	}
	TEST_ASSERT_EQUAL_PTR(fw_ptr(DIE_STATE_TABLE_ADDR), dieStateTablePtr);
	TEST_ASSERT_EQUAL_PTR(fw_ptr(WAY_PRIORITY_TABLE_ADDR), wayPriorityTablePtr);
}

/* ------------------------------------------------------------------------ */
/* generic put / selective-get behaviour for all seven lists                 */
/* ------------------------------------------------------------------------ */

static void check_list_put_appends_in_order(const list_desc_t *l)
{
	unsigned int ways[USER_WAYS];

	l->clear(TEST_CH);
	TEST_ASSERT_EQUAL_UINT_MESSAGE(0, walk_list(l, TEST_CH, ways), l->name);

	l->put(TEST_CH, 4);
	TEST_ASSERT_EQUAL_UINT_MESSAGE(1, walk_list(l, TEST_CH, ways), l->name);
	TEST_ASSERT_EQUAL_UINT_MESSAGE(4, ways[0], l->name);
	TEST_ASSERT_EQUAL_UINT_MESSAGE(4, l->head(TEST_CH), l->name);
	TEST_ASSERT_EQUAL_UINT_MESSAGE(4, l->tail(TEST_CH), l->name);

	l->put(TEST_CH, 1);
	l->put(TEST_CH, 6);
	TEST_ASSERT_EQUAL_UINT_MESSAGE(3, walk_list(l, TEST_CH, ways), l->name);
	TEST_ASSERT_EQUAL_UINT_MESSAGE(4, ways[0], l->name);
	TEST_ASSERT_EQUAL_UINT_MESSAGE(1, ways[1], l->name);
	TEST_ASSERT_EQUAL_UINT_MESSAGE(6, ways[2], l->name);
	TEST_ASSERT_EQUAL_UINT_MESSAGE(4, l->head(TEST_CH), l->name);
	TEST_ASSERT_EQUAL_UINT_MESSAGE(6, l->tail(TEST_CH), l->name);
}

static void check_list_get_removes_middle_tail_head_and_last(const list_desc_t *l)
{
	unsigned int ways[USER_WAYS];

	l->clear(TEST_CH);
	l->put(TEST_CH, 0);
	l->put(TEST_CH, 1);
	l->put(TEST_CH, 2);
	l->put(TEST_CH, 3);

	/* middle */
	l->get(TEST_CH, 1);
	TEST_ASSERT_EQUAL_UINT_MESSAGE(3, walk_list(l, TEST_CH, ways), l->name);
	TEST_ASSERT_EQUAL_UINT_MESSAGE(0, ways[0], l->name);
	TEST_ASSERT_EQUAL_UINT_MESSAGE(2, ways[1], l->name);
	TEST_ASSERT_EQUAL_UINT_MESSAGE(3, ways[2], l->name);

	/* tail */
	l->get(TEST_CH, 3);
	TEST_ASSERT_EQUAL_UINT_MESSAGE(2, walk_list(l, TEST_CH, ways), l->name);
	TEST_ASSERT_EQUAL_UINT_MESSAGE(2, l->tail(TEST_CH), l->name);

	/* head */
	l->get(TEST_CH, 0);
	TEST_ASSERT_EQUAL_UINT_MESSAGE(1, walk_list(l, TEST_CH, ways), l->name);
	TEST_ASSERT_EQUAL_UINT_MESSAGE(2, l->head(TEST_CH), l->name);
	TEST_ASSERT_EQUAL_UINT_MESSAGE(2, l->tail(TEST_CH), l->name);

	/* last element */
	l->get(TEST_CH, 2);
	TEST_ASSERT_EQUAL_UINT_MESSAGE(0, walk_list(l, TEST_CH, ways), l->name);
	TEST_ASSERT_EQUAL_UINT_MESSAGE(WAY_NONE, l->head(TEST_CH), l->name);
	TEST_ASSERT_EQUAL_UINT_MESSAGE(WAY_NONE, l->tail(TEST_CH), l->name);
}

static void check_list_remove_then_reinsert_goes_to_tail(const list_desc_t *l)
{
	unsigned int ways[USER_WAYS];

	l->clear(TEST_CH);
	l->put(TEST_CH, 5);
	l->put(TEST_CH, 6);
	l->put(TEST_CH, 7);
	l->get(TEST_CH, 5);
	l->put(TEST_CH, 5);

	TEST_ASSERT_EQUAL_UINT_MESSAGE(3, walk_list(l, TEST_CH, ways), l->name);
	TEST_ASSERT_EQUAL_UINT_MESSAGE(6, ways[0], l->name);
	TEST_ASSERT_EQUAL_UINT_MESSAGE(7, ways[1], l->name);
	TEST_ASSERT_EQUAL_UINT_MESSAGE(5, ways[2], l->name);
}

static void test_every_list_put_appends_in_order(void)
{
	unsigned int i;

	for (i = 0; i < LIST_COUNT; i++)
		check_list_put_appends_in_order(&lists[i]);
}

static void test_every_list_selective_get_handles_middle_tail_head_last(void)
{
	unsigned int i;

	for (i = 0; i < LIST_COUNT; i++)
		check_list_get_removes_middle_tail_head_and_last(&lists[i]);
}

static void test_every_list_reinsert_after_removal_appends_to_tail(void)
{
	unsigned int i;

	for (i = 0; i < LIST_COUNT; i++)
		check_list_remove_then_reinsert_goes_to_tail(&lists[i]);
}

static void test_lists_are_independent_per_channel(void)
{
	unsigned int ways[USER_WAYS];

	if (USER_CHANNELS < 2)
		TEST_IGNORE_MESSAGE("needs GREEDYFTL_TEST_CHANNELS >= 2");

	PutToNandWriteList(0, 2);
	PutToNandWriteList(1, 5);

	TEST_ASSERT_EQUAL_UINT(1, walk_list(&lists[3], 0, ways));
	TEST_ASSERT_EQUAL_UINT(2, ways[0]);
	TEST_ASSERT_EQUAL_UINT(1, walk_list(&lists[3], 1, ways));
	TEST_ASSERT_EQUAL_UINT(5, ways[0]);
}

static void test_selective_get_from_full_idle_list(void)
{
	unsigned int ways[USER_WAYS];
	unsigned int i;

	/* fresh init: idle list = 0..USER_WAYS-1 */
	SelectivGetFromNandIdleList(TEST_CH, 3);          /* middle */
	SelectivGetFromNandIdleList(TEST_CH, USER_WAYS - 1); /* tail */
	SelectivGetFromNandIdleList(TEST_CH, 0);          /* head */

	TEST_ASSERT_EQUAL_UINT(USER_WAYS - 3, walk_list(&lists[0], TEST_CH, ways));
	for (i = 0; i < USER_WAYS - 3; i++) {
		TEST_ASSERT_NOT_EQUAL(0, ways[i]);
		TEST_ASSERT_NOT_EQUAL(3, ways[i]);
		TEST_ASSERT_NOT_EQUAL(USER_WAYS - 1, ways[i]);
	}
	TEST_ASSERT_EQUAL_UINT(1, P(TEST_CH).idleHead);
	TEST_ASSERT_EQUAL_UINT(USER_WAYS - 2, P(TEST_CH).idleTail);
}

/* ------------------------------------------------------------------------ */
/* PutToNandWayPriorityTable dispatch                                        */
/* ------------------------------------------------------------------------ */

static unsigned int alloc_req_with_code(unsigned int reqCode)
{
	unsigned int reqSlotTag = GetFromFreeReqQ();

	req(reqSlotTag)->reqCode = reqCode;
	return reqSlotTag;
}

static void test_way_priority_table_dispatches_by_req_code(void)
{
	PutToNandWayPriorityTable(alloc_req_with_code(REQ_CODE_READ), TEST_CH, 0);
	PutToNandWayPriorityTable(alloc_req_with_code(REQ_CODE_READ_TRANSFER), TEST_CH, 1);
	PutToNandWayPriorityTable(alloc_req_with_code(REQ_CODE_WRITE), TEST_CH, 2);
	PutToNandWayPriorityTable(alloc_req_with_code(REQ_CODE_ERASE), TEST_CH, 3);
	PutToNandWayPriorityTable(alloc_req_with_code(REQ_CODE_RESET), TEST_CH, 4);
	PutToNandWayPriorityTable(alloc_req_with_code(REQ_CODE_SET_FEATURE), TEST_CH, 5);

	TEST_ASSERT_EQUAL_UINT(0, P(TEST_CH).readTriggerHead);
	TEST_ASSERT_EQUAL_UINT(1, P(TEST_CH).readTransferHead);
	TEST_ASSERT_EQUAL_UINT(2, P(TEST_CH).writeHead);
	TEST_ASSERT_EQUAL_UINT(3, P(TEST_CH).eraseHead);
	TEST_ASSERT_EQUAL_UINT(5, P(TEST_CH).writeTail);
	TEST_ASSERT_EQUAL_UINT(4, dieStateTablePtr->dieState[TEST_CH][2].nextWay);
	TEST_ASSERT_EQUAL_UINT(5, dieStateTablePtr->dieState[TEST_CH][4].nextWay);
	TEST_ASSERT_EQUAL_UINT(WAY_NONE, P(TEST_CH).statusCheckHead);
}

static void test_way_priority_table_rejects_dma_req_codes(void)
{
	FW_EXPECT_ASSERT(PutToNandWayPriorityTable(alloc_req_with_code(REQ_CODE_RxDMA), TEST_CH, 0));
	FW_EXPECT_ASSERT(PutToNandWayPriorityTable(alloc_req_with_code(REQ_CODE_TxDMA), TEST_CH, 0));
	FW_EXPECT_ASSERT(PutToNandWayPriorityTable(alloc_req_with_code(REQ_CODE_FLUSH), TEST_CH, 0));
}

/* ------------------------------------------------------------------------ */
/* GenerateNandRowAddr                                                       */
/* ------------------------------------------------------------------------ */

static unsigned int alloc_nand_req(void)
{
	unsigned int reqSlotTag = GetFromFreeReqQ();
	SSD_REQ_FORMAT *r = req(reqSlotTag);

	r->reqType = REQ_TYPE_NAND;
	r->reqCode = REQ_CODE_READ;
	r->reqOpt.dataBufFormat = REQ_OPT_DATA_BUF_NONE;
	return reqSlotTag;
}

static void test_row_addr_phy_org_total_block_space(void)
{
	unsigned int reqSlotTag = alloc_nand_req();
	SSD_REQ_FORMAT *r = req(reqSlotTag);

	r->reqOpt.nandAddr = REQ_OPT_NAND_ADDR_PHY_ORG;
	r->reqOpt.blockSpace = REQ_OPT_BLOCK_SPACE_TOTAL;
	r->nandInfo.physicalCh = TEST_CH;
	r->nandInfo.physicalWay = 1;
	r->nandInfo.physicalBlock = 12;
	r->nandInfo.physicalPage = 34;
	TEST_ASSERT_EQUAL_HEX32(LUN_0_BASE_ADDR + 12 * PAGES_PER_MLC_BLOCK + 34, GenerateNandRowAddr(reqSlotTag));

	r->nandInfo.physicalBlock = TOTAL_BLOCKS_PER_LUN + 12;
	TEST_ASSERT_EQUAL_HEX32(LUN_1_BASE_ADDR + 12 * PAGES_PER_MLC_BLOCK + 34, GenerateNandRowAddr(reqSlotTag));
}

static void test_row_addr_phy_org_main_block_space_uses_remap(void)
{
	unsigned int reqSlotTag = alloc_nand_req();
	SSD_REQ_FORMAT *r = req(reqSlotTag);
	unsigned int dieNo = Pcw2VdieTranslation(TEST_CH, 2);

	r->reqOpt.nandAddr = REQ_OPT_NAND_ADDR_PHY_ORG;
	r->reqOpt.blockSpace = REQ_OPT_BLOCK_SPACE_MAIN;
	r->nandInfo.physicalCh = TEST_CH;
	r->nandInfo.physicalWay = 2;
	r->nandInfo.physicalBlock = 20;
	r->nandInfo.physicalPage = 7;

	/* identity remap after init */
	TEST_ASSERT_EQUAL_UINT(20, phyBlockMapPtr->phyBlock[dieNo][20].remappedPhyBlock);
	TEST_ASSERT_EQUAL_HEX32(LUN_0_BASE_ADDR + 20 * PAGES_PER_MLC_BLOCK + 7, GenerateNandRowAddr(reqSlotTag));

	/* remapped block is used instead of the requested one */
	phyBlockMapPtr->phyBlock[dieNo][20].remappedPhyBlock = MAIN_BLOCKS_PER_LUN + 3;
	TEST_ASSERT_EQUAL_HEX32(LUN_0_BASE_ADDR + (MAIN_BLOCKS_PER_LUN + 3) * PAGES_PER_MLC_BLOCK + 7,
			GenerateNandRowAddr(reqSlotTag));

	/* second LUN: block index wraps at MAIN_BLOCKS_PER_LUN, lun offset applied */
	r->nandInfo.physicalBlock = MAIN_BLOCKS_PER_LUN + 20;
	phyBlockMapPtr->phyBlock[dieNo][TOTAL_BLOCKS_PER_LUN + 20].remappedPhyBlock = TOTAL_BLOCKS_PER_LUN + 20;
	TEST_ASSERT_EQUAL_HEX32(LUN_1_BASE_ADDR + 20 * PAGES_PER_MLC_BLOCK + 7, GenerateNandRowAddr(reqSlotTag));
}

static void test_row_addr_vsa_translates_through_block_map_and_slc_page(void)
{
	unsigned int reqSlotTag = alloc_nand_req();
	SSD_REQ_FORMAT *r = req(reqSlotTag);
	unsigned int dieNo = 1 % USER_DIES;
	unsigned int vblock = 9;
	unsigned int vpage = 4;
	unsigned int expectPage = (BITS_PER_FLASH_CELL == SLC_MODE) ? Vpage2PlsbPageTranslation(vpage) : vpage;

	r->reqOpt.nandAddr = REQ_OPT_NAND_ADDR_VSA;
	r->nandInfo.virtualSliceAddr = Vorg2VsaTranslation(dieNo, vblock, vpage);

	TEST_ASSERT_EQUAL_HEX32(LUN_0_BASE_ADDR + Vblock2PblockOfTbsTranslation(vblock) * PAGES_PER_MLC_BLOCK + expectPage,
			GenerateNandRowAddr(reqSlotTag));

	/* page 0 stays page 0 in SLC mode */
	r->nandInfo.virtualSliceAddr = Vorg2VsaTranslation(dieNo, vblock, 0);
	TEST_ASSERT_EQUAL_HEX32(LUN_0_BASE_ADDR + Vblock2PblockOfTbsTranslation(vblock) * PAGES_PER_MLC_BLOCK,
			GenerateNandRowAddr(reqSlotTag));

	/* a virtual block in the second LUN */
	r->nandInfo.virtualSliceAddr = Vorg2VsaTranslation(dieNo, USER_BLOCKS_PER_LUN + 2, 0);
	TEST_ASSERT_EQUAL_HEX32(LUN_1_BASE_ADDR + 2 * PAGES_PER_MLC_BLOCK, GenerateNandRowAddr(reqSlotTag));
}

static void test_row_addr_rejects_unknown_nand_addr_option(void)
{
	unsigned int reqSlotTag = alloc_nand_req();

	req(reqSlotTag)->reqOpt.nandAddr = 0x3;
	FW_EXPECT_ASSERT(GenerateNandRowAddr(reqSlotTag));
}

/* ------------------------------------------------------------------------ */
/* GenerateDataBufAddr / GenerateSpareDataBufAddr                            */
/* ------------------------------------------------------------------------ */

static void test_data_buf_addr_for_nand_requests(void)
{
	unsigned int reqSlotTag = alloc_nand_req();
	SSD_REQ_FORMAT *r = req(reqSlotTag);

	r->dataBufInfo.entry = 7;
	r->reqOpt.dataBufFormat = REQ_OPT_DATA_BUF_ENTRY;
	TEST_ASSERT_EQUAL_HEX32(DATA_BUFFER_BASE_ADDR + 7 * BYTES_PER_DATA_REGION_OF_SLICE, GenerateDataBufAddr(reqSlotTag));
	TEST_ASSERT_EQUAL_HEX32(SPARE_DATA_BUFFER_BASE_ADDR + 7 * BYTES_PER_SPARE_REGION_OF_SLICE,
			GenerateSpareDataBufAddr(reqSlotTag));

	r->reqOpt.dataBufFormat = REQ_OPT_DATA_BUF_TEMP_ENTRY;
	TEST_ASSERT_EQUAL_HEX32(TEMPORARY_DATA_BUFFER_BASE_ADDR + 7 * BYTES_PER_DATA_REGION_OF_SLICE,
			GenerateDataBufAddr(reqSlotTag));
	TEST_ASSERT_EQUAL_HEX32(TEMPORARY_SPARE_DATA_BUFFER_BASE_ADDR + 7 * BYTES_PER_SPARE_REGION_OF_SLICE,
			GenerateSpareDataBufAddr(reqSlotTag));

	r->reqOpt.dataBufFormat = REQ_OPT_DATA_BUF_ADDR;
	r->dataBufInfo.addr = 0x12345000;
	TEST_ASSERT_EQUAL_HEX32(0x12345000, GenerateDataBufAddr(reqSlotTag));
	TEST_ASSERT_EQUAL_HEX32(0x12345000 + BYTES_PER_DATA_REGION_OF_SLICE, GenerateSpareDataBufAddr(reqSlotTag));

	r->reqOpt.dataBufFormat = REQ_OPT_DATA_BUF_NONE;
	TEST_ASSERT_EQUAL_HEX32(RESERVED_DATA_BUFFER_BASE_ADDR, GenerateDataBufAddr(reqSlotTag));
	TEST_ASSERT_EQUAL_HEX32(RESERVED_DATA_BUFFER_BASE_ADDR + BYTES_PER_DATA_REGION_OF_SLICE,
			GenerateSpareDataBufAddr(reqSlotTag));
}

static void test_data_buf_addr_for_nvme_dma_requests_adds_block_offset(void)
{
	unsigned int reqSlotTag = GetFromFreeReqQ();
	SSD_REQ_FORMAT *r = req(reqSlotTag);

	r->reqType = REQ_TYPE_NVME_DMA;
	r->reqOpt.dataBufFormat = REQ_OPT_DATA_BUF_ENTRY;
	r->dataBufInfo.entry = 2;
	r->nvmeDmaInfo.nvmeBlockOffset = 3;

	TEST_ASSERT_EQUAL_HEX32(DATA_BUFFER_BASE_ADDR + 2 * BYTES_PER_DATA_REGION_OF_SLICE + 3 * BYTES_PER_NVME_BLOCK,
			GenerateDataBufAddr(reqSlotTag));
	TEST_ASSERT_EQUAL_HEX32(SPARE_DATA_BUFFER_BASE_ADDR + 2 * BYTES_PER_SPARE_REGION_OF_SLICE,
			GenerateSpareDataBufAddr(reqSlotTag));
}

static void test_data_buf_addr_rejects_bad_format_for_nvme_dma(void)
{
	unsigned int reqSlotTag = GetFromFreeReqQ();
	SSD_REQ_FORMAT *r = req(reqSlotTag);

	r->reqType = REQ_TYPE_NVME_DMA;
	r->reqOpt.dataBufFormat = REQ_OPT_DATA_BUF_TEMP_ENTRY;
	FW_EXPECT_ASSERT(GenerateDataBufAddr(reqSlotTag));
	FW_EXPECT_ASSERT(GenerateSpareDataBufAddr(reqSlotTag));

	r->reqOpt.dataBufFormat = REQ_OPT_DATA_BUF_ADDR;
	FW_EXPECT_ASSERT(GenerateDataBufAddr(reqSlotTag));
	FW_EXPECT_ASSERT(GenerateSpareDataBufAddr(reqSlotTag));
}

static void test_data_buf_addr_rejects_slice_req_type(void)
{
	unsigned int reqSlotTag = GetFromFreeReqQ();
	SSD_REQ_FORMAT *r = req(reqSlotTag);

	r->reqType = REQ_TYPE_SLICE;
	r->reqOpt.dataBufFormat = REQ_OPT_DATA_BUF_ENTRY;
	FW_EXPECT_ASSERT(GenerateDataBufAddr(reqSlotTag));
	FW_EXPECT_ASSERT(GenerateSpareDataBufAddr(reqSlotTag));
}

int main(void)
{
	UNITY_BEGIN();
	RUN_TEST(test_init_puts_every_way_of_every_channel_in_idle_list);

	RUN_TEST(test_every_list_put_appends_in_order);
	RUN_TEST(test_every_list_selective_get_handles_middle_tail_head_last);
	RUN_TEST(test_every_list_reinsert_after_removal_appends_to_tail);
	RUN_TEST(test_lists_are_independent_per_channel);
	RUN_TEST(test_selective_get_from_full_idle_list);

	RUN_TEST(test_way_priority_table_dispatches_by_req_code);
	RUN_TEST(test_way_priority_table_rejects_dma_req_codes);

	RUN_TEST(test_row_addr_phy_org_total_block_space);
	RUN_TEST(test_row_addr_phy_org_main_block_space_uses_remap);
	RUN_TEST(test_row_addr_vsa_translates_through_block_map_and_slc_page);
	RUN_TEST(test_row_addr_rejects_unknown_nand_addr_option);

	RUN_TEST(test_data_buf_addr_for_nand_requests);
	RUN_TEST(test_data_buf_addr_for_nvme_dma_requests_adds_block_offset);
	RUN_TEST(test_data_buf_addr_rejects_bad_format_for_nvme_dma);
	RUN_TEST(test_data_buf_addr_rejects_slice_req_type);
	return UNITY_END();
}
