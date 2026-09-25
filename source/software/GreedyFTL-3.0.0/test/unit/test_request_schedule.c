/* Unit tests for request_schedule.c: die state machine, way priority lists,
 * NAND request issue / status check / retry handling and the per-channel
 * scheduler. All NAND traffic goes through the mock_nsc controller model. */
#include <string.h>
#include "unity.h"
#include "ftl_test_env.h"
#include "memory_map.h"
#include "address_translation.h"
#include "request_allocation.h"
#include "request_transform.h"
#include "request_schedule.h"

/* request_schedule.h declares dieStatusTablePtr but the definition is dieStateTablePtr. */
extern P_DIE_STATE_TABLE dieStateTablePtr;

#define CH 0
#define WAY 3
#define TEST_BLOCK 17
#define TEST_PAGE 5
#define MAX_SCHEDULER_ITERATIONS 64

/* errorInfo0 with CRC valid, spare valid and 21 worst-chunk bit errors (> threshold). */
#define ERRINFO0_WARNING (MOCK_NSC_ERRINFO0_CLEAN | ((BIT_ERROR_THRESHOLD_PER_CHUNK + 1) << 16))
/* errorInfo0 with the CRC-valid bit cleared -> uncorrectable. */
#define ERRINFO0_CRC_FAIL 0x01000000u
/* Way mask with only way WAY busy. */
#define READY_BUSY_ALL_BUT_WAY (~(1u << WAY))

static P_DIE_STATE_ENTRY die(unsigned int ch, unsigned int way)
{
	return &dieStateTablePtr->dieState[ch][way];
}

static P_WAY_PRIORITY_ENTRY prio(unsigned int ch)
{
	return &wayPriorityTablePtr->wayPriority[ch];
}

/* Allocate a request slot addressed by physical (ch, way, block, page) in the
 * total block space and append it to nandReqQ[ch][way]. */
static unsigned int enqueue_nand_req(unsigned int ch, unsigned int way, unsigned int reqCode,
                                     unsigned int block, unsigned int page)
{
	unsigned int tag = GetFromFreeReqQ();
	P_SSD_REQ_FORMAT req = &reqPoolPtr->reqPool[tag];

	req->reqType = REQ_TYPE_NAND;
	req->reqCode = reqCode;
	req->reqOpt.dataBufFormat = REQ_OPT_DATA_BUF_ADDR;
	req->reqOpt.nandAddr = REQ_OPT_NAND_ADDR_PHY_ORG;
	req->reqOpt.blockSpace = REQ_OPT_BLOCK_SPACE_TOTAL;
	req->reqOpt.nandEcc = REQ_OPT_NAND_ECC_ON;
	req->reqOpt.nandEccWarning = REQ_OPT_NAND_ECC_WARNING_ON;
	req->reqOpt.rowAddrDependencyCheck = REQ_OPT_ROW_ADDR_DEPENDENCY_NONE;
	req->dataBufInfo.addr = RESERVED_DATA_BUFFER_BASE_ADDR;
	req->nandInfo.physicalCh = ch;
	req->nandInfo.physicalWay = way;
	req->nandInfo.physicalBlock = block;
	req->nandInfo.physicalPage = page;

	PutToNandReqQ(tag, ch, way);
	return tag;
}

static unsigned int enqueue_default_req(unsigned int reqCode)
{
	return enqueue_nand_req(CH, WAY, reqCode, TEST_BLOCK, TEST_PAGE);
}

static unsigned int expected_row_addr(unsigned int block, unsigned int page)
{
	return block * PAGES_PER_MLC_BLOCK + page;
}

/* Take every way of a channel off the idle list so list tests start empty. */
static void clear_idle_list(unsigned int ch)
{
	unsigned int way;

	for (way = 0; way < USER_WAYS; way++)
		SelectivGetFromNandIdleList(ch, way);
	TEST_ASSERT_EQUAL_UINT(WAY_NONE, prio(ch)->idleHead);
	TEST_ASSERT_EQUAL_UINT(WAY_NONE, prio(ch)->idleTail);
}

static void run_scheduler_until_idle(void)
{
	unsigned int i;

	for (i = 0; i < MAX_SCHEDULER_ITERATIONS && notCompletedNandReqCnt; i++)
		SchedulingNandReq();
	TEST_ASSERT_EQUAL_UINT_MESSAGE(0, notCompletedNandReqCnt, "scheduler did not drain the NAND queues");
}

static const mock_nsc_call_t *nth_op(mock_nsc_op_t op, size_t n)
{
	size_t i, seen = 0;

	for (i = 0; i < mock_nsc_call_count(); i++) {
		const mock_nsc_call_t *c = mock_nsc_call_at(i);
		if (c && c->op == op && seen++ == n)
			return c;
	}
	return NULL;
}

/* InitFTL() is run once (it scans the whole mocked NAND array); every test
 * then restores the state it can touch: request pool/queues and scheduler
 * tables via the firmware's own init functions, the block maps that grown-bad
 * handling marks, the data buffer the pseudo-bad mark is written to, and the
 * NAND controller mock. */
static PHY_BLOCK_MAP bootPhyBlockMap;
static BAD_BLOCK_TABLE_INFO_MAP bootBbtInfoMap;
static unsigned char bootReservedBuf[BYTES_PER_DATA_REGION_OF_SLICE];

static void boot_once(void)
{
	ftl_test_env_init_ftl();
	memcpy(&bootPhyBlockMap, phyBlockMapPtr, sizeof(bootPhyBlockMap));
	memcpy(&bootBbtInfoMap, bbtInfoMapPtr, sizeof(bootBbtInfoMap));
	memcpy(bootReservedBuf, (void *)RESERVED_DATA_BUFFER_BASE_ADDR, sizeof(bootReservedBuf));
}

void setUp(void)
{
	InitReqPool();
	InitReqScheduler();
	memcpy(phyBlockMapPtr, &bootPhyBlockMap, sizeof(bootPhyBlockMap));
	memcpy(bbtInfoMapPtr, &bootBbtInfoMap, sizeof(bootBbtInfoMap));
	memcpy((void *)RESERVED_DATA_BUFFER_BASE_ADDR, bootReservedBuf, sizeof(bootReservedBuf));
	mock_nsc_reset();
}

void tearDown(void) {}

/* ------------------------------------------------------------------------ */
/* InitReqScheduler                                                         */
/* ------------------------------------------------------------------------ */

static void test_all_dies_idle_after_boot(void)
{
	unsigned int ch, way;

	for (ch = 0; ch < USER_CHANNELS; ch++) {
		TEST_ASSERT_EQUAL_UINT(0, prio(ch)->idleHead);
		TEST_ASSERT_EQUAL_UINT(USER_WAYS - 1, prio(ch)->idleTail);
		TEST_ASSERT_EQUAL_UINT(WAY_NONE, prio(ch)->statusReportHead);
		TEST_ASSERT_EQUAL_UINT(WAY_NONE, prio(ch)->readTriggerHead);
		TEST_ASSERT_EQUAL_UINT(WAY_NONE, prio(ch)->writeHead);
		TEST_ASSERT_EQUAL_UINT(WAY_NONE, prio(ch)->readTransferHead);
		TEST_ASSERT_EQUAL_UINT(WAY_NONE, prio(ch)->eraseHead);
		TEST_ASSERT_EQUAL_UINT(WAY_NONE, prio(ch)->statusCheckHead);
		for (way = 0; way < USER_WAYS; way++) {
			TEST_ASSERT_EQUAL_UINT(DIE_STATE_IDLE, die(ch, way)->dieState);
			TEST_ASSERT_EQUAL_UINT(REQ_STATUS_CHECK_OPT_NONE, die(ch, way)->reqStatusCheckOpt);
			TEST_ASSERT_EQUAL_INT(RETRY_LIMIT, retryLimitTablePtr->retryLimit[ch][way]);
		}
	}
	TEST_ASSERT_EQUAL_UINT(0, notCompletedNandReqCnt);
}

static void test_idle_list_is_doubly_linked_in_way_order_after_boot(void)
{
	unsigned int way;

	TEST_ASSERT_EQUAL_UINT(WAY_NONE, die(CH, 0)->prevWay);
	for (way = 0; way + 1 < USER_WAYS; way++) {
		TEST_ASSERT_EQUAL_UINT(way + 1, die(CH, way)->nextWay);
		TEST_ASSERT_EQUAL_UINT(way, die(CH, way + 1)->prevWay);
	}
	TEST_ASSERT_EQUAL_UINT(WAY_NONE, die(CH, USER_WAYS - 1)->nextWay);
}

/* ------------------------------------------------------------------------ */
/* Way priority lists                                                       */
/* ------------------------------------------------------------------------ */

typedef void (*list_op_t)(unsigned int, unsigned int);

typedef struct {
	const char *name;
	list_op_t put;
	list_op_t get;
	/* offsets into WAY_PRIORITY_ENTRY are bitfields, so read through accessors */
	unsigned int (*head)(unsigned int ch);
	unsigned int (*tail)(unsigned int ch);
} list_desc_t;

#define DEFINE_LIST_ACCESSORS(field)                                                 \
	static unsigned int field##_head(unsigned int ch) { return prio(ch)->field##Head; } \
	static unsigned int field##_tail(unsigned int ch) { return prio(ch)->field##Tail; }

DEFINE_LIST_ACCESSORS(idle)
DEFINE_LIST_ACCESSORS(statusReport)
DEFINE_LIST_ACCESSORS(readTrigger)
DEFINE_LIST_ACCESSORS(write)
DEFINE_LIST_ACCESSORS(readTransfer)
DEFINE_LIST_ACCESSORS(erase)
DEFINE_LIST_ACCESSORS(statusCheck)

static const list_desc_t lists[] = {
	{ "idle", PutToNandIdleList, SelectivGetFromNandIdleList, idle_head, idle_tail },
	{ "statusReport", PutToNandStatusReportList, SelectivGetFromNandStatusReportList, statusReport_head, statusReport_tail },
	{ "readTrigger", PutToNandReadTriggerList, SelectiveGetFromNandReadTriggerList, readTrigger_head, readTrigger_tail },
	{ "write", PutToNandWriteList, SelectiveGetFromNandWriteList, write_head, write_tail },
	{ "readTransfer", PutToNandReadTransferList, SelectiveGetFromNandReadTransferList, readTransfer_head, readTransfer_tail },
	{ "erase", PutToNandEraseList, SelectiveGetFromNandEraseList, erase_head, erase_tail },
	{ "statusCheck", PutToNandStatusCheckList, SelectiveGetFromNandStatusCheckList, statusCheck_head, statusCheck_tail },
};

static void check_list_put_and_selective_get(const list_desc_t *l)
{
	clear_idle_list(CH);

	/* put: first element becomes head and tail */
	l->put(CH, 2);
	TEST_ASSERT_EQUAL_UINT_MESSAGE(2, l->head(CH), l->name);
	TEST_ASSERT_EQUAL_UINT_MESSAGE(2, l->tail(CH), l->name);
	TEST_ASSERT_EQUAL_UINT_MESSAGE(WAY_NONE, die(CH, 2)->prevWay, l->name);
	TEST_ASSERT_EQUAL_UINT_MESSAGE(WAY_NONE, die(CH, 2)->nextWay, l->name);

	/* put: later elements append at the tail */
	l->put(CH, 5);
	l->put(CH, 7);
	TEST_ASSERT_EQUAL_UINT_MESSAGE(2, l->head(CH), l->name);
	TEST_ASSERT_EQUAL_UINT_MESSAGE(7, l->tail(CH), l->name);
	TEST_ASSERT_EQUAL_UINT_MESSAGE(5, die(CH, 2)->nextWay, l->name);
	TEST_ASSERT_EQUAL_UINT_MESSAGE(2, die(CH, 5)->prevWay, l->name);
	TEST_ASSERT_EQUAL_UINT_MESSAGE(7, die(CH, 5)->nextWay, l->name);
	TEST_ASSERT_EQUAL_UINT_MESSAGE(5, die(CH, 7)->prevWay, l->name);

	/* get from the middle relinks neighbours */
	l->get(CH, 5);
	TEST_ASSERT_EQUAL_UINT_MESSAGE(7, die(CH, 2)->nextWay, l->name);
	TEST_ASSERT_EQUAL_UINT_MESSAGE(2, die(CH, 7)->prevWay, l->name);
	TEST_ASSERT_EQUAL_UINT_MESSAGE(2, l->head(CH), l->name);
	TEST_ASSERT_EQUAL_UINT_MESSAGE(7, l->tail(CH), l->name);

	/* get from the tail moves the tail back */
	l->get(CH, 7);
	TEST_ASSERT_EQUAL_UINT_MESSAGE(WAY_NONE, die(CH, 2)->nextWay, l->name);
	TEST_ASSERT_EQUAL_UINT_MESSAGE(2, l->tail(CH), l->name);

	/* get from the head moves the head forward */
	l->put(CH, 4);
	l->get(CH, 2);
	TEST_ASSERT_EQUAL_UINT_MESSAGE(4, l->head(CH), l->name);
	TEST_ASSERT_EQUAL_UINT_MESSAGE(WAY_NONE, die(CH, 4)->prevWay, l->name);

	/* get of the last element empties the list */
	l->get(CH, 4);
	TEST_ASSERT_EQUAL_UINT_MESSAGE(WAY_NONE, l->head(CH), l->name);
	TEST_ASSERT_EQUAL_UINT_MESSAGE(WAY_NONE, l->tail(CH), l->name);
}

static void test_every_way_list_supports_put_and_selective_get(void)
{
	size_t i;

	for (i = 0; i < sizeof(lists) / sizeof(lists[0]); i++)
		check_list_put_and_selective_get(&lists[i]);
}

static void test_way_lists_are_independent_per_channel(void)
{
	clear_idle_list(0);
	clear_idle_list(1);

	PutToNandWriteList(0, 1);
	PutToNandWriteList(1, 6);

	TEST_ASSERT_EQUAL_UINT(1, prio(0)->writeHead);
	TEST_ASSERT_EQUAL_UINT(6, prio(1)->writeHead);
	SelectiveGetFromNandWriteList(0, 1);
	TEST_ASSERT_EQUAL_UINT(WAY_NONE, prio(0)->writeHead);
	TEST_ASSERT_EQUAL_UINT(6, prio(1)->writeHead);
}

static void test_put_to_way_priority_table_routes_by_req_code(void)
{
	unsigned int tag;

	clear_idle_list(CH);

	tag = enqueue_nand_req(CH, 0, REQ_CODE_READ, TEST_BLOCK, 0);
	PutToNandWayPriorityTable(tag, CH, 0);
	TEST_ASSERT_EQUAL_UINT(0, prio(CH)->readTriggerHead);

	tag = enqueue_nand_req(CH, 1, REQ_CODE_READ_TRANSFER, TEST_BLOCK, 0);
	PutToNandWayPriorityTable(tag, CH, 1);
	TEST_ASSERT_EQUAL_UINT(1, prio(CH)->readTransferHead);

	tag = enqueue_nand_req(CH, 2, REQ_CODE_WRITE, TEST_BLOCK, 0);
	PutToNandWayPriorityTable(tag, CH, 2);
	TEST_ASSERT_EQUAL_UINT(2, prio(CH)->writeHead);

	tag = enqueue_nand_req(CH, 3, REQ_CODE_ERASE, TEST_BLOCK, 0);
	PutToNandWayPriorityTable(tag, CH, 3);
	TEST_ASSERT_EQUAL_UINT(3, prio(CH)->eraseHead);

	/* RESET and SET_FEATURE share the write list */
	tag = enqueue_nand_req(CH, 4, REQ_CODE_RESET, TEST_BLOCK, 0);
	PutToNandWayPriorityTable(tag, CH, 4);
	tag = enqueue_nand_req(CH, 5, REQ_CODE_SET_FEATURE, TEST_BLOCK, 0);
	PutToNandWayPriorityTable(tag, CH, 5);
	TEST_ASSERT_EQUAL_UINT(2, prio(CH)->writeHead);
	TEST_ASSERT_EQUAL_UINT(5, prio(CH)->writeTail);
	TEST_ASSERT_EQUAL_UINT(4, die(CH, 2)->nextWay);
	TEST_ASSERT_EQUAL_UINT(5, die(CH, 4)->nextWay);
}

static void test_put_to_way_priority_table_asserts_on_unknown_req_code(void)
{
	unsigned int tag = enqueue_default_req(REQ_CODE_FLUSH);

	FTL_TEST_EXPECT_ASSERT(PutToNandWayPriorityTable(tag, CH, WAY));
}

/* ------------------------------------------------------------------------ */
/* Address generation                                                       */
/* ------------------------------------------------------------------------ */

static void test_row_addr_phy_total_block_space_lun0_and_lun1(void)
{
	unsigned int tag = enqueue_default_req(REQ_CODE_READ);

	TEST_ASSERT_EQUAL_HEX32(LUN_0_BASE_ADDR + expected_row_addr(TEST_BLOCK, TEST_PAGE), GenerateNandRowAddr(tag));

	reqPoolPtr->reqPool[tag].nandInfo.physicalBlock = TOTAL_BLOCKS_PER_LUN + TEST_BLOCK;
	TEST_ASSERT_EQUAL_HEX32(LUN_1_BASE_ADDR + expected_row_addr(TEST_BLOCK, TEST_PAGE), GenerateNandRowAddr(tag));
}

static void test_row_addr_phy_main_block_space_follows_remap(void)
{
	unsigned int tag = enqueue_default_req(REQ_CODE_READ);
	unsigned int dieNo = Pcw2VdieTranslation(CH, WAY);
	unsigned int remapped = MAIN_BLOCKS_PER_LUN + 3; /* a spare block in the extended area */

	reqPoolPtr->reqPool[tag].reqOpt.blockSpace = REQ_OPT_BLOCK_SPACE_MAIN;
	phyBlockMapPtr->phyBlock[dieNo][TEST_BLOCK].remappedPhyBlock = remapped;

	TEST_ASSERT_EQUAL_HEX32(LUN_0_BASE_ADDR + expected_row_addr(remapped, TEST_PAGE), GenerateNandRowAddr(tag));

	/* main block in LUN 1 maps to the LUN 1 slot of the phy block map */
	reqPoolPtr->reqPool[tag].nandInfo.physicalBlock = MAIN_BLOCKS_PER_LUN + TEST_BLOCK;
	phyBlockMapPtr->phyBlock[dieNo][TOTAL_BLOCKS_PER_LUN + TEST_BLOCK].remappedPhyBlock = TOTAL_BLOCKS_PER_LUN + TEST_BLOCK;
	TEST_ASSERT_EQUAL_HEX32(LUN_1_BASE_ADDR + expected_row_addr(TEST_BLOCK, TEST_PAGE), GenerateNandRowAddr(tag));
}

static void test_row_addr_vsa_uses_slc_lsb_page_translation(void)
{
	unsigned int tag = enqueue_default_req(REQ_CODE_READ);
	unsigned int dieNo = Pcw2VdieTranslation(CH, WAY);
	unsigned int vBlock = 9, vPage = 4;
	unsigned int expectedPage = Vpage2PlsbPageTranslation(vPage);

	reqPoolPtr->reqPool[tag].reqOpt.nandAddr = REQ_OPT_NAND_ADDR_VSA;
	reqPoolPtr->reqPool[tag].nandInfo.virtualSliceAddr = Vorg2VsaTranslation(dieNo, vBlock, vPage);
	phyBlockMapPtr->phyBlock[dieNo][vBlock].remappedPhyBlock = vBlock;

	TEST_ASSERT_EQUAL_HEX32(LUN_0_BASE_ADDR + expected_row_addr(vBlock, expectedPage), GenerateNandRowAddr(tag));
}

static void test_row_addr_asserts_on_unknown_nand_addr_option(void)
{
	unsigned int tag = enqueue_default_req(REQ_CODE_READ);

	reqPoolPtr->reqPool[tag].reqOpt.nandAddr = 2;
	FTL_TEST_EXPECT_ASSERT(GenerateNandRowAddr(tag));
}

static void test_data_and_spare_buf_addr_for_each_nand_buffer_format(void)
{
	unsigned int tag = enqueue_default_req(REQ_CODE_WRITE);
	P_SSD_REQ_FORMAT req = &reqPoolPtr->reqPool[tag];

	req->reqOpt.dataBufFormat = REQ_OPT_DATA_BUF_ENTRY;
	req->dataBufInfo.entry = 3;
	TEST_ASSERT_EQUAL_HEX32(DATA_BUFFER_BASE_ADDR + 3 * BYTES_PER_DATA_REGION_OF_SLICE, GenerateDataBufAddr(tag));
	TEST_ASSERT_EQUAL_HEX32(SPARE_DATA_BUFFER_BASE_ADDR + 3 * BYTES_PER_SPARE_REGION_OF_SLICE, GenerateSpareDataBufAddr(tag));

	req->reqOpt.dataBufFormat = REQ_OPT_DATA_BUF_TEMP_ENTRY;
	req->dataBufInfo.entry = 2;
	TEST_ASSERT_EQUAL_HEX32(TEMPORARY_DATA_BUFFER_BASE_ADDR + 2 * BYTES_PER_DATA_REGION_OF_SLICE, GenerateDataBufAddr(tag));
	TEST_ASSERT_EQUAL_HEX32(TEMPORARY_SPARE_DATA_BUFFER_BASE_ADDR + 2 * BYTES_PER_SPARE_REGION_OF_SLICE, GenerateSpareDataBufAddr(tag));

	req->reqOpt.dataBufFormat = REQ_OPT_DATA_BUF_ADDR;
	req->dataBufInfo.addr = 0x12340000;
	TEST_ASSERT_EQUAL_HEX32(0x12340000, GenerateDataBufAddr(tag));
	TEST_ASSERT_EQUAL_HEX32(0x12340000 + BYTES_PER_DATA_REGION_OF_SLICE, GenerateSpareDataBufAddr(tag));

	req->reqOpt.dataBufFormat = REQ_OPT_DATA_BUF_NONE;
	TEST_ASSERT_EQUAL_HEX32(RESERVED_DATA_BUFFER_BASE_ADDR, GenerateDataBufAddr(tag));
	TEST_ASSERT_EQUAL_HEX32(RESERVED_DATA_BUFFER_BASE_ADDR + BYTES_PER_DATA_REGION_OF_SLICE, GenerateSpareDataBufAddr(tag));
}

static void test_data_buf_addr_for_nvme_dma_adds_block_offset(void)
{
	unsigned int tag = enqueue_default_req(REQ_CODE_RxDMA);
	P_SSD_REQ_FORMAT req = &reqPoolPtr->reqPool[tag];

	req->reqType = REQ_TYPE_NVME_DMA;
	req->reqOpt.dataBufFormat = REQ_OPT_DATA_BUF_ENTRY;
	req->dataBufInfo.entry = 1;
	req->nvmeDmaInfo.nvmeBlockOffset = 2;

	TEST_ASSERT_EQUAL_HEX32(DATA_BUFFER_BASE_ADDR + BYTES_PER_DATA_REGION_OF_SLICE + 2 * BYTES_PER_NVME_BLOCK,
	                        GenerateDataBufAddr(tag));
	TEST_ASSERT_EQUAL_HEX32(SPARE_DATA_BUFFER_BASE_ADDR + BYTES_PER_SPARE_REGION_OF_SLICE, GenerateSpareDataBufAddr(tag));

	req->reqOpt.dataBufFormat = REQ_OPT_DATA_BUF_ADDR;
	FTL_TEST_EXPECT_ASSERT(GenerateDataBufAddr(tag));
	FTL_TEST_EXPECT_ASSERT(GenerateSpareDataBufAddr(tag));
}

static void test_buf_addr_asserts_on_slice_req_type(void)
{
	unsigned int tag = enqueue_default_req(REQ_CODE_WRITE);

	reqPoolPtr->reqPool[tag].reqType = REQ_TYPE_SLICE;
	FTL_TEST_EXPECT_ASSERT(GenerateDataBufAddr(tag));
	FTL_TEST_EXPECT_ASSERT(GenerateSpareDataBufAddr(tag));
}

/* ------------------------------------------------------------------------ */
/* IssueNandReq                                                             */
/* ------------------------------------------------------------------------ */

static void test_issue_read_triggers_page_read_and_arms_status_check(void)
{
	const mock_nsc_call_t *c;

	enqueue_default_req(REQ_CODE_READ);
	IssueNandReq(CH, WAY);

	TEST_ASSERT_EQUAL_UINT(REQ_STATUS_CHECK_OPT_CHECK, die(CH, WAY)->reqStatusCheckOpt);
	c = nth_op(MOCK_NSC_OP_READ_TRIGGER, 0);
	TEST_ASSERT_NOT_NULL(c);
	TEST_ASSERT_EQUAL_PTR(chCtlReg[CH], c->dev);
	TEST_ASSERT_EQUAL_INT(WAY, c->way);
	TEST_ASSERT_EQUAL_HEX32(expected_row_addr(TEST_BLOCK, TEST_PAGE), c->rowAddress);
}

static void test_issue_read_transfer_with_ecc_fills_error_info_and_completion(void)
{
	const mock_nsc_call_t *c;

	enqueue_default_req(REQ_CODE_READ_TRANSFER);
	mock_nsc_set_transfer_result(chCtlReg[CH], WAY, 1, ERRINFO0_WARNING, MOCK_NSC_ERRINFO1_CLEAN);
	IssueNandReq(CH, WAY);

	TEST_ASSERT_EQUAL_UINT(REQ_STATUS_CHECK_OPT_COMPLETION_FLAG, die(CH, WAY)->reqStatusCheckOpt);
	c = nth_op(MOCK_NSC_OP_READ_TRANSFER, 0);
	TEST_ASSERT_NOT_NULL(c);
	TEST_ASSERT_EQUAL_PTR((void *)(uintptr_t)RESERVED_DATA_BUFFER_BASE_ADDR, c->dataBuf);
	TEST_ASSERT_EQUAL_PTR((void *)(uintptr_t)(RESERVED_DATA_BUFFER_BASE_ADDR + BYTES_PER_DATA_REGION_OF_SLICE), c->spareBuf);
	TEST_ASSERT_EQUAL_HEX32(1, completeFlagTablePtr->completeFlag[CH][WAY]);
	TEST_ASSERT_EQUAL_HEX32(ERRINFO0_WARNING, eccErrorInfoTablePtr->errorInfo[CH][WAY][0]);
	TEST_ASSERT_EQUAL_HEX32(MOCK_NSC_ERRINFO1_CLEAN, eccErrorInfoTablePtr->errorInfo[CH][WAY][1]);
}

static void test_issue_read_transfer_without_ecc_uses_raw_transfer(void)
{
	unsigned int tag = enqueue_default_req(REQ_CODE_READ_TRANSFER);

	reqPoolPtr->reqPool[tag].reqOpt.nandEcc = REQ_OPT_NAND_ECC_OFF;
	IssueNandReq(CH, WAY);

	TEST_ASSERT_EQUAL_UINT(REQ_STATUS_CHECK_OPT_COMPLETION_FLAG, die(CH, WAY)->reqStatusCheckOpt);
	TEST_ASSERT_EQUAL_size_t(1, mock_nsc_count_op(MOCK_NSC_OP_READ_TRANSFER_RAW));
	TEST_ASSERT_EQUAL_size_t(0, mock_nsc_count_op(MOCK_NSC_OP_READ_TRANSFER));
	TEST_ASSERT_EQUAL_HEX32(1, completeFlagTablePtr->completeFlag[CH][WAY]);
}

static void test_issue_write_programs_page_with_data_and_spare(void)
{
	const mock_nsc_call_t *c;

	enqueue_default_req(REQ_CODE_WRITE);
	IssueNandReq(CH, WAY);

	TEST_ASSERT_EQUAL_UINT(REQ_STATUS_CHECK_OPT_CHECK, die(CH, WAY)->reqStatusCheckOpt);
	c = nth_op(MOCK_NSC_OP_PROGRAM, 0);
	TEST_ASSERT_NOT_NULL(c);
	TEST_ASSERT_EQUAL_INT(WAY, c->way);
	TEST_ASSERT_EQUAL_HEX32(expected_row_addr(TEST_BLOCK, TEST_PAGE), c->rowAddress);
	TEST_ASSERT_EQUAL_PTR((void *)(uintptr_t)RESERVED_DATA_BUFFER_BASE_ADDR, c->dataBuf);
	TEST_ASSERT_NOT_NULL(c->spareBuf);
}

static void test_issue_erase_erases_block(void)
{
	const mock_nsc_call_t *c;

	enqueue_default_req(REQ_CODE_ERASE);
	IssueNandReq(CH, WAY);

	TEST_ASSERT_EQUAL_UINT(REQ_STATUS_CHECK_OPT_CHECK, die(CH, WAY)->reqStatusCheckOpt);
	c = nth_op(MOCK_NSC_OP_ERASE, 0);
	TEST_ASSERT_NOT_NULL(c);
	TEST_ASSERT_EQUAL_HEX32(expected_row_addr(TEST_BLOCK, TEST_PAGE), c->rowAddress);
}

static void test_issue_reset_and_set_feature_are_synchronous(void)
{
	const mock_nsc_call_t *c;

	enqueue_default_req(REQ_CODE_RESET);
	die(CH, WAY)->reqStatusCheckOpt = REQ_STATUS_CHECK_OPT_CHECK;
	IssueNandReq(CH, WAY);
	TEST_ASSERT_EQUAL_UINT(REQ_STATUS_CHECK_OPT_NONE, die(CH, WAY)->reqStatusCheckOpt);
	TEST_ASSERT_EQUAL_size_t(1, mock_nsc_count_op(MOCK_NSC_OP_RESET));

	enqueue_nand_req(CH, WAY + 1, REQ_CODE_SET_FEATURE, TEST_BLOCK, 0);
	die(CH, WAY + 1)->reqStatusCheckOpt = REQ_STATUS_CHECK_OPT_CHECK;
	IssueNandReq(CH, WAY + 1);
	TEST_ASSERT_EQUAL_UINT(REQ_STATUS_CHECK_OPT_NONE, die(CH, WAY + 1)->reqStatusCheckOpt);
	c = nth_op(MOCK_NSC_OP_SET_FEATURES, 0);
	TEST_ASSERT_NOT_NULL(c);
	TEST_ASSERT_EQUAL_INT(WAY + 1, c->way);
	TEST_ASSERT_EQUAL_PTR((void *)(uintptr_t)TEMPORARY_PAY_LOAD_ADDR, c->dataBuf);
}

static void test_issue_asserts_on_unknown_req_code(void)
{
	enqueue_default_req(REQ_CODE_FLUSH);
	FTL_TEST_EXPECT_ASSERT(IssueNandReq(CH, WAY));
}

/* ------------------------------------------------------------------------ */
/* CheckReqStatus / CheckEccErrorInfo                                       */
/* ------------------------------------------------------------------------ */

static void test_check_status_opt_check_issues_status_check_and_moves_to_report(void)
{
	enqueue_default_req(REQ_CODE_WRITE);
	die(CH, WAY)->reqStatusCheckOpt = REQ_STATUS_CHECK_OPT_CHECK;
	mock_nsc_set_status_report(chCtlReg[CH], WAY, MOCK_NSC_STATUS_PASS);

	TEST_ASSERT_EQUAL_UINT(REQ_STATUS_RUNNING, CheckReqStatus(CH, WAY));
	TEST_ASSERT_EQUAL_UINT(REQ_STATUS_CHECK_OPT_REPORT, die(CH, WAY)->reqStatusCheckOpt);
	TEST_ASSERT_EQUAL_size_t(1, mock_nsc_count_op(MOCK_NSC_OP_STATUS_CHECK));
	TEST_ASSERT_EQUAL_HEX32(MOCK_NSC_STATUS_PASS, statusReportTablePtr->statusReport[CH][WAY]);
}

static void test_check_status_report_pass_returns_done_and_clears_opt(void)
{
	enqueue_default_req(REQ_CODE_WRITE);
	die(CH, WAY)->reqStatusCheckOpt = REQ_STATUS_CHECK_OPT_REPORT;
	statusReportTablePtr->statusReport[CH][WAY] = MOCK_NSC_STATUS_PASS;

	TEST_ASSERT_EQUAL_UINT(REQ_STATUS_DONE, CheckReqStatus(CH, WAY));
	TEST_ASSERT_EQUAL_UINT(REQ_STATUS_CHECK_OPT_NONE, die(CH, WAY)->reqStatusCheckOpt);
}

static void test_check_status_report_fail_returns_fail(void)
{
	enqueue_default_req(REQ_CODE_WRITE);
	die(CH, WAY)->reqStatusCheckOpt = REQ_STATUS_CHECK_OPT_REPORT;
	statusReportTablePtr->statusReport[CH][WAY] = MOCK_NSC_STATUS_FAIL;

	TEST_ASSERT_EQUAL_UINT(REQ_STATUS_FAIL, CheckReqStatus(CH, WAY));
	TEST_ASSERT_EQUAL_UINT(REQ_STATUS_CHECK_OPT_REPORT, die(CH, WAY)->reqStatusCheckOpt);
}

static void test_check_status_report_pending_reissues_status_check(void)
{
	enqueue_default_req(REQ_CODE_WRITE);
	die(CH, WAY)->reqStatusCheckOpt = REQ_STATUS_CHECK_OPT_REPORT;
	statusReportTablePtr->statusReport[CH][WAY] = MOCK_NSC_STATUS_PENDING;

	TEST_ASSERT_EQUAL_UINT(REQ_STATUS_RUNNING, CheckReqStatus(CH, WAY));
	TEST_ASSERT_EQUAL_UINT(REQ_STATUS_CHECK_OPT_CHECK, die(CH, WAY)->reqStatusCheckOpt);
}

static void test_check_status_report_not_yet_written_keeps_waiting(void)
{
	enqueue_default_req(REQ_CODE_WRITE);
	die(CH, WAY)->reqStatusCheckOpt = REQ_STATUS_CHECK_OPT_REPORT;
	statusReportTablePtr->statusReport[CH][WAY] = 0;

	TEST_ASSERT_EQUAL_UINT(REQ_STATUS_RUNNING, CheckReqStatus(CH, WAY));
	TEST_ASSERT_EQUAL_UINT(REQ_STATUS_CHECK_OPT_REPORT, die(CH, WAY)->reqStatusCheckOpt);
}

static void test_check_status_completion_flag_clear_is_running(void)
{
	enqueue_default_req(REQ_CODE_READ_TRANSFER);
	die(CH, WAY)->reqStatusCheckOpt = REQ_STATUS_CHECK_OPT_COMPLETION_FLAG;
	completeFlagTablePtr->completeFlag[CH][WAY] = 0;

	TEST_ASSERT_EQUAL_UINT(REQ_STATUS_RUNNING, CheckReqStatus(CH, WAY));
}

static void test_check_status_completion_with_clean_ecc_is_done(void)
{
	enqueue_default_req(REQ_CODE_READ_TRANSFER);
	IssueNandReq(CH, WAY); /* mock fills completion + clean error info */

	TEST_ASSERT_EQUAL_UINT(REQ_STATUS_DONE, CheckReqStatus(CH, WAY));
}

static void test_check_status_completion_with_uncorrectable_ecc_is_fail(void)
{
	enqueue_default_req(REQ_CODE_READ_TRANSFER);
	mock_nsc_set_transfer_result(chCtlReg[CH], WAY, 1, ERRINFO0_CRC_FAIL, MOCK_NSC_ERRINFO1_CLEAN);
	IssueNandReq(CH, WAY);

	TEST_ASSERT_EQUAL_UINT(REQ_STATUS_FAIL, CheckReqStatus(CH, WAY));
}

static void test_check_status_completion_with_many_bit_errors_is_warning(void)
{
	enqueue_default_req(REQ_CODE_READ_TRANSFER);
	mock_nsc_set_transfer_result(chCtlReg[CH], WAY, 1, ERRINFO0_WARNING, MOCK_NSC_ERRINFO1_CLEAN);
	IssueNandReq(CH, WAY);

	TEST_ASSERT_EQUAL_UINT(REQ_STATUS_WARNING, CheckReqStatus(CH, WAY));
}

static void test_check_status_completion_ignores_ecc_when_ecc_off(void)
{
	unsigned int tag = enqueue_default_req(REQ_CODE_READ_TRANSFER);

	reqPoolPtr->reqPool[tag].reqOpt.nandEcc = REQ_OPT_NAND_ECC_OFF;
	mock_nsc_set_transfer_result(chCtlReg[CH], WAY, 1, ERRINFO0_CRC_FAIL, 0);
	IssueNandReq(CH, WAY);

	TEST_ASSERT_EQUAL_UINT(REQ_STATUS_DONE, CheckReqStatus(CH, WAY));
}

static void test_check_status_opt_none_follows_ready_busy(void)
{
	enqueue_default_req(REQ_CODE_RESET);
	die(CH, WAY)->reqStatusCheckOpt = REQ_STATUS_CHECK_OPT_NONE;

	mock_nsc_set_ready_busy(chCtlReg[CH], READY_BUSY_ALL_BUT_WAY);
	TEST_ASSERT_EQUAL_UINT(REQ_STATUS_RUNNING, CheckReqStatus(CH, WAY));

	mock_nsc_set_ready_busy(chCtlReg[CH], 0xffffffffu);
	TEST_ASSERT_EQUAL_UINT(REQ_STATUS_DONE, CheckReqStatus(CH, WAY));
}

static void test_check_status_asserts_on_unknown_option(void)
{
	enqueue_default_req(REQ_CODE_WRITE);
	die(CH, WAY)->reqStatusCheckOpt = 7;
	FTL_TEST_EXPECT_ASSERT(CheckReqStatus(CH, WAY));
}

static void test_ecc_error_info_classification(void)
{
	unsigned int tag = enqueue_default_req(REQ_CODE_READ_TRANSFER);
	unsigned int *info = eccErrorInfoTablePtr->errorInfo[CH][WAY];

	info[0] = MOCK_NSC_ERRINFO0_CLEAN;
	info[1] = MOCK_NSC_ERRINFO1_CLEAN;
	TEST_ASSERT_EQUAL_UINT(ERROR_INFO_PASS, CheckEccErrorInfo(CH, WAY));

	/* exactly the threshold is still a pass */
	info[0] = MOCK_NSC_ERRINFO0_CLEAN | (BIT_ERROR_THRESHOLD_PER_CHUNK << 16);
	TEST_ASSERT_EQUAL_UINT(ERROR_INFO_PASS, CheckEccErrorInfo(CH, WAY));

	info[0] = ERRINFO0_WARNING;
	TEST_ASSERT_EQUAL_UINT(ERROR_INFO_WARNING, CheckEccErrorInfo(CH, WAY));

	reqPoolPtr->reqPool[tag].reqOpt.nandEccWarning = REQ_OPT_NAND_ECC_WARNING_OFF;
	TEST_ASSERT_EQUAL_UINT(ERROR_INFO_PASS, CheckEccErrorInfo(CH, WAY));

	info[0] = ERRINFO0_CRC_FAIL;
	TEST_ASSERT_EQUAL_UINT(ERROR_INFO_FAIL, CheckEccErrorInfo(CH, WAY));

	info[0] = 0x10000000u; /* CRC ok, spare chunk invalid */
	TEST_ASSERT_EQUAL_UINT(ERROR_INFO_FAIL, CheckEccErrorInfo(CH, WAY));

	info[0] = MOCK_NSC_ERRINFO0_CLEAN;
	info[1] = 0xfffffffeu; /* one page chunk invalid */
	TEST_ASSERT_EQUAL_UINT(ERROR_INFO_FAIL, CheckEccErrorInfo(CH, WAY));
}

/* ------------------------------------------------------------------------ */
/* ExecuteNandReq: die state machine                                        */
/* ------------------------------------------------------------------------ */

static void test_execute_on_idle_die_issues_request_and_enters_exe(void)
{
	enqueue_default_req(REQ_CODE_WRITE);

	ExecuteNandReq(CH, WAY, REQ_STATUS_RUNNING);

	TEST_ASSERT_EQUAL_UINT(DIE_STATE_EXE, die(CH, WAY)->dieState);
	TEST_ASSERT_EQUAL_size_t(1, mock_nsc_count_op(MOCK_NSC_OP_PROGRAM));
	TEST_ASSERT_EQUAL_UINT(1, notCompletedNandReqCnt);
}

static void test_execute_running_on_exe_die_does_nothing(void)
{
	enqueue_default_req(REQ_CODE_WRITE);
	ExecuteNandReq(CH, WAY, REQ_STATUS_RUNNING);
	mock_nsc_reset();

	ExecuteNandReq(CH, WAY, REQ_STATUS_RUNNING);

	TEST_ASSERT_EQUAL_UINT(DIE_STATE_EXE, die(CH, WAY)->dieState);
	TEST_ASSERT_EQUAL_size_t(0, mock_nsc_call_count());
	TEST_ASSERT_EQUAL_UINT(1, notCompletedNandReqCnt);
}

static void test_execute_done_write_completes_request_and_resets_retry_limit(void)
{
	unsigned int tag = enqueue_default_req(REQ_CODE_WRITE);
	unsigned int freeBefore = freeReqQ.reqCnt;

	ExecuteNandReq(CH, WAY, REQ_STATUS_RUNNING);
	retryLimitTablePtr->retryLimit[CH][WAY] = 1;

	ExecuteNandReq(CH, WAY, REQ_STATUS_DONE);

	TEST_ASSERT_EQUAL_UINT(DIE_STATE_IDLE, die(CH, WAY)->dieState);
	TEST_ASSERT_EQUAL_UINT(REQ_SLOT_TAG_NONE, nandReqQ[CH][WAY].headReq);
	TEST_ASSERT_EQUAL_UINT(0, notCompletedNandReqCnt);
	TEST_ASSERT_EQUAL_UINT(freeBefore + 1, freeReqQ.reqCnt);
	TEST_ASSERT_EQUAL_UINT(REQ_QUEUE_TYPE_FREE, reqPoolPtr->reqPool[tag].reqQueueType);
	TEST_ASSERT_EQUAL_INT(RETRY_LIMIT, retryLimitTablePtr->retryLimit[CH][WAY]);
}

static void test_execute_done_read_trigger_converts_to_read_transfer(void)
{
	unsigned int tag = enqueue_default_req(REQ_CODE_READ);

	ExecuteNandReq(CH, WAY, REQ_STATUS_RUNNING);
	ExecuteNandReq(CH, WAY, REQ_STATUS_DONE);

	TEST_ASSERT_EQUAL_UINT(DIE_STATE_IDLE, die(CH, WAY)->dieState);
	TEST_ASSERT_EQUAL_UINT(REQ_CODE_READ_TRANSFER, reqPoolPtr->reqPool[tag].reqCode);
	TEST_ASSERT_EQUAL_UINT(tag, nandReqQ[CH][WAY].headReq);
	TEST_ASSERT_EQUAL_UINT(1, notCompletedNandReqCnt);
}

static void test_execute_failed_read_transfer_retries_as_read_trigger(void)
{
	unsigned int tag = enqueue_default_req(REQ_CODE_READ_TRANSFER);
	unsigned int dieNo = Pcw2VdieTranslation(CH, WAY);

	ExecuteNandReq(CH, WAY, REQ_STATUS_RUNNING);
	ExecuteNandReq(CH, WAY, REQ_STATUS_FAIL);

	TEST_ASSERT_EQUAL_INT(RETRY_LIMIT - 1, retryLimitTablePtr->retryLimit[CH][WAY]);
	TEST_ASSERT_EQUAL_UINT(REQ_CODE_READ, reqPoolPtr->reqPool[tag].reqCode);
	TEST_ASSERT_EQUAL_UINT(DIE_STATE_IDLE, die(CH, WAY)->dieState);
	TEST_ASSERT_EQUAL_UINT(tag, nandReqQ[CH][WAY].headReq);
	TEST_ASSERT_EQUAL_UINT(1, notCompletedNandReqCnt);
	TEST_ASSERT_NOT_EQUAL(BLOCK_STATE_BAD, phyBlockMapPtr->phyBlock[dieNo][TEST_BLOCK].bad);
}

static void test_execute_failed_read_trigger_retries_and_keeps_code(void)
{
	unsigned int tag = enqueue_default_req(REQ_CODE_READ);

	ExecuteNandReq(CH, WAY, REQ_STATUS_RUNNING);
	ExecuteNandReq(CH, WAY, REQ_STATUS_FAIL);

	TEST_ASSERT_EQUAL_INT(RETRY_LIMIT - 1, retryLimitTablePtr->retryLimit[CH][WAY]);
	TEST_ASSERT_EQUAL_UINT(REQ_CODE_READ, reqPoolPtr->reqPool[tag].reqCode);
	TEST_ASSERT_EQUAL_UINT(tag, nandReqQ[CH][WAY].headReq);
}

static void test_execute_failed_read_with_exhausted_retries_marks_grown_bad_block(void)
{
	enqueue_default_req(REQ_CODE_READ_TRANSFER);
	unsigned int dieNo = Pcw2VdieTranslation(CH, WAY);

	ExecuteNandReq(CH, WAY, REQ_STATUS_RUNNING);
	retryLimitTablePtr->retryLimit[CH][WAY] = 0;

	ExecuteNandReq(CH, WAY, REQ_STATUS_FAIL);

	TEST_ASSERT_EQUAL_UINT(BLOCK_STATE_BAD, phyBlockMapPtr->phyBlock[dieNo][TEST_BLOCK].bad);
	TEST_ASSERT_EQUAL_UINT(BBT_INFO_GROWN_BAD_UPDATE_BOOKED, bbtInfoMapPtr->bbtInfo[dieNo].grownBadUpdate);
	TEST_ASSERT_EQUAL_INT(RETRY_LIMIT, retryLimitTablePtr->retryLimit[CH][WAY]);
	TEST_ASSERT_EQUAL_UINT(REQ_SLOT_TAG_NONE, nandReqQ[CH][WAY].headReq);
	TEST_ASSERT_EQUAL_UINT(DIE_STATE_IDLE, die(CH, WAY)->dieState);
	TEST_ASSERT_EQUAL_UINT(0, notCompletedNandReqCnt);
}

static void test_execute_failed_write_is_not_retried_and_marks_grown_bad_block(void)
{
	enqueue_default_req(REQ_CODE_WRITE);
	unsigned int dieNo = Pcw2VdieTranslation(CH, WAY);

	ExecuteNandReq(CH, WAY, REQ_STATUS_RUNNING);
	ExecuteNandReq(CH, WAY, REQ_STATUS_FAIL);

	TEST_ASSERT_EQUAL_UINT(BLOCK_STATE_BAD, phyBlockMapPtr->phyBlock[dieNo][TEST_BLOCK].bad);
	TEST_ASSERT_EQUAL_INT(RETRY_LIMIT, retryLimitTablePtr->retryLimit[CH][WAY]);
	TEST_ASSERT_EQUAL_UINT(REQ_SLOT_TAG_NONE, nandReqQ[CH][WAY].headReq);
	TEST_ASSERT_EQUAL_UINT(0, notCompletedNandReqCnt);
}

static void test_execute_failed_erase_in_lun1_marks_correct_block(void)
{
	enqueue_nand_req(CH, WAY, REQ_CODE_ERASE, TOTAL_BLOCKS_PER_LUN + TEST_BLOCK, 0);
	unsigned int dieNo = Pcw2VdieTranslation(CH, WAY);

	ExecuteNandReq(CH, WAY, REQ_STATUS_RUNNING);
	ExecuteNandReq(CH, WAY, REQ_STATUS_FAIL);

	TEST_ASSERT_EQUAL_UINT(BLOCK_STATE_BAD, phyBlockMapPtr->phyBlock[dieNo][TOTAL_BLOCKS_PER_LUN + TEST_BLOCK].bad);
	TEST_ASSERT_NOT_EQUAL(BLOCK_STATE_BAD, phyBlockMapPtr->phyBlock[dieNo][TEST_BLOCK].bad);
}

static void test_execute_failed_raw_read_into_addr_buffer_writes_pseudo_bad_mark(void)
{
	unsigned int tag = enqueue_default_req(REQ_CODE_READ_TRANSFER);
	unsigned char *buf = (unsigned char *)(uintptr_t)RESERVED_DATA_BUFFER_BASE_ADDR;

	reqPoolPtr->reqPool[tag].reqOpt.nandEcc = REQ_OPT_NAND_ECC_OFF;
	*buf = 0xFF;
	ExecuteNandReq(CH, WAY, REQ_STATUS_RUNNING);
	retryLimitTablePtr->retryLimit[CH][WAY] = 0;

	ExecuteNandReq(CH, WAY, REQ_STATUS_FAIL);

	TEST_ASSERT_EQUAL_HEX8(PSEUDO_BAD_BLOCK_MARK, *buf);
}

static void test_execute_failed_read_with_ecc_on_leaves_data_buffer_alone(void)
{
	enqueue_default_req(REQ_CODE_READ_TRANSFER);
	unsigned char *buf = (unsigned char *)(uintptr_t)RESERVED_DATA_BUFFER_BASE_ADDR;

	*buf = 0xFF;
	ExecuteNandReq(CH, WAY, REQ_STATUS_RUNNING);
	retryLimitTablePtr->retryLimit[CH][WAY] = 0;

	ExecuteNandReq(CH, WAY, REQ_STATUS_FAIL);

	TEST_ASSERT_EQUAL_HEX8(0xFF, *buf);
}

static void test_execute_warning_completes_request_and_marks_grown_bad_block(void)
{
	enqueue_default_req(REQ_CODE_READ_TRANSFER);
	unsigned int dieNo = Pcw2VdieTranslation(CH, WAY);

	ExecuteNandReq(CH, WAY, REQ_STATUS_RUNNING);
	ExecuteNandReq(CH, WAY, REQ_STATUS_WARNING);

	TEST_ASSERT_EQUAL_UINT(BLOCK_STATE_BAD, phyBlockMapPtr->phyBlock[dieNo][TEST_BLOCK].bad);
	TEST_ASSERT_EQUAL_UINT(BBT_INFO_GROWN_BAD_UPDATE_BOOKED, bbtInfoMapPtr->bbtInfo[dieNo].grownBadUpdate);
	TEST_ASSERT_EQUAL_UINT(REQ_SLOT_TAG_NONE, nandReqQ[CH][WAY].headReq);
	TEST_ASSERT_EQUAL_UINT(DIE_STATE_IDLE, die(CH, WAY)->dieState);
	TEST_ASSERT_EQUAL_UINT(0, notCompletedNandReqCnt);
}

static void test_execute_asserts_on_unknown_status(void)
{
	enqueue_default_req(REQ_CODE_WRITE);
	ExecuteNandReq(CH, WAY, REQ_STATUS_RUNNING);

	FTL_TEST_EXPECT_ASSERT(ExecuteNandReq(CH, WAY, 9));
}

/* ------------------------------------------------------------------------ */
/* SchedulingNandReqPerCh                                                   */
/* ------------------------------------------------------------------------ */

static void test_scheduler_leaves_idle_channel_untouched(void)
{
	SchedulingNandReqPerCh(CH);

	TEST_ASSERT_EQUAL_UINT(0, prio(CH)->idleHead);
	TEST_ASSERT_EQUAL_UINT(USER_WAYS - 1, prio(CH)->idleTail);
	TEST_ASSERT_EQUAL_size_t(0, mock_nsc_count_op(MOCK_NSC_OP_PROGRAM));
	TEST_ASSERT_EQUAL_size_t(0, mock_nsc_count_op(MOCK_NSC_OP_STATUS_CHECK));
}

static void test_scheduler_moves_write_through_status_check_and_report_lists(void)
{
	enqueue_default_req(REQ_CODE_WRITE);

	/* pass 1: idle -> write list -> issued -> statusCheck list */
	SchedulingNandReqPerCh(CH);
	TEST_ASSERT_EQUAL_UINT(DIE_STATE_EXE, die(CH, WAY)->dieState);
	TEST_ASSERT_EQUAL_size_t(1, mock_nsc_count_op(MOCK_NSC_OP_PROGRAM));
	TEST_ASSERT_EQUAL_UINT(WAY_NONE, prio(CH)->writeHead);
	TEST_ASSERT_EQUAL_UINT(WAY, prio(CH)->statusCheckHead);
	TEST_ASSERT_EQUAL_UINT(WAY_NONE, die(CH, WAY)->prevWay);
	TEST_ASSERT_EQUAL_UINT(REQ_STATUS_CHECK_OPT_CHECK, die(CH, WAY)->reqStatusCheckOpt);

	/* pass 2: statusCheck list -> status check issued -> statusReport list */
	SchedulingNandReqPerCh(CH);
	TEST_ASSERT_EQUAL_size_t(1, mock_nsc_count_op(MOCK_NSC_OP_STATUS_CHECK));
	TEST_ASSERT_EQUAL_UINT(WAY_NONE, prio(CH)->statusCheckHead);
	TEST_ASSERT_EQUAL_UINT(WAY, prio(CH)->statusReportHead);
	TEST_ASSERT_EQUAL_UINT(REQ_STATUS_CHECK_OPT_REPORT, die(CH, WAY)->reqStatusCheckOpt);
	TEST_ASSERT_EQUAL_UINT(1, notCompletedNandReqCnt);

	/* pass 3: report says pass -> request done -> way back on idle list */
	SchedulingNandReqPerCh(CH);
	TEST_ASSERT_EQUAL_UINT(0, notCompletedNandReqCnt);
	TEST_ASSERT_EQUAL_UINT(DIE_STATE_IDLE, die(CH, WAY)->dieState);
	TEST_ASSERT_EQUAL_UINT(WAY_NONE, prio(CH)->statusReportHead);
	TEST_ASSERT_EQUAL_UINT(WAY, prio(CH)->idleTail);
}

static void test_scheduler_issues_lists_in_priority_order(void)
{
	const mock_nsc_call_t *c;
	size_t i, n = 0;
	mock_nsc_op_t order[4];

	/* enqueue in the reverse of the expected issue order */
	enqueue_nand_req(CH, 0, REQ_CODE_READ_TRANSFER, TEST_BLOCK, 0);
	enqueue_nand_req(CH, 1, REQ_CODE_WRITE, TEST_BLOCK, 0);
	enqueue_nand_req(CH, 2, REQ_CODE_ERASE, TEST_BLOCK, 0);
	enqueue_nand_req(CH, 3, REQ_CODE_READ, TEST_BLOCK, 0);

	SchedulingNandReqPerCh(CH);

	for (i = 0; i < mock_nsc_call_count() && n < 4; i++) {
		c = mock_nsc_call_at(i);
		if (c->op == MOCK_NSC_OP_READ_TRIGGER || c->op == MOCK_NSC_OP_ERASE ||
		    c->op == MOCK_NSC_OP_PROGRAM || c->op == MOCK_NSC_OP_READ_TRANSFER)
			order[n++] = c->op;
	}
	TEST_ASSERT_EQUAL_size_t(4, n);
	TEST_ASSERT_EQUAL_INT(MOCK_NSC_OP_READ_TRIGGER, order[0]);
	TEST_ASSERT_EQUAL_INT(MOCK_NSC_OP_ERASE, order[1]);
	TEST_ASSERT_EQUAL_INT(MOCK_NSC_OP_PROGRAM, order[2]);
	TEST_ASSERT_EQUAL_INT(MOCK_NSC_OP_READ_TRANSFER, order[3]);

	/* read transfer completes via completion flag: it waits on the report list, not statusCheck */
	TEST_ASSERT_EQUAL_UINT(0, prio(CH)->statusReportHead);
	TEST_ASSERT_EQUAL_UINT(3, prio(CH)->statusCheckHead);
	TEST_ASSERT_EQUAL_UINT(1, prio(CH)->statusCheckTail);
}

static void test_scheduler_takes_ways_off_idle_list_only_when_they_have_work(void)
{
	enqueue_nand_req(CH, 2, REQ_CODE_WRITE, TEST_BLOCK, 0);
	enqueue_nand_req(CH, 6, REQ_CODE_WRITE, TEST_BLOCK, 0);

	SchedulingNandReqPerCh(CH);

	TEST_ASSERT_EQUAL_UINT(0, prio(CH)->idleHead);
	TEST_ASSERT_EQUAL_UINT(USER_WAYS - 1, prio(CH)->idleTail);
	TEST_ASSERT_EQUAL_UINT(3, die(CH, 1)->nextWay);
	TEST_ASSERT_EQUAL_UINT(1, die(CH, 3)->prevWay);
	TEST_ASSERT_EQUAL_UINT(7, die(CH, 5)->nextWay);
	TEST_ASSERT_EQUAL_UINT(5, die(CH, 7)->prevWay);
	TEST_ASSERT_EQUAL_UINT(2, prio(CH)->statusCheckHead);
	TEST_ASSERT_EQUAL_UINT(6, prio(CH)->writeHead);
}

/* Each per-code list walk continues via dieState[way].nextWay *after* the way
 * was unlinked (nextWay == WAY_NONE), so a scheduling pass issues at most one
 * request per list and picks up the remaining ways on the following passes. */
static void test_scheduler_issues_one_request_per_list_per_pass(void)
{
	enqueue_nand_req(CH, 2, REQ_CODE_WRITE, TEST_BLOCK, 0);
	enqueue_nand_req(CH, 6, REQ_CODE_WRITE, TEST_BLOCK, 0);

	SchedulingNandReqPerCh(CH);
	TEST_ASSERT_EQUAL_size_t(1, mock_nsc_count_op(MOCK_NSC_OP_PROGRAM));
	TEST_ASSERT_EQUAL_UINT(2, prio(CH)->statusCheckTail);
	TEST_ASSERT_EQUAL_UINT(6, prio(CH)->writeTail);

	SchedulingNandReqPerCh(CH);
	TEST_ASSERT_EQUAL_size_t(2, mock_nsc_count_op(MOCK_NSC_OP_PROGRAM));
	TEST_ASSERT_EQUAL_UINT(WAY_NONE, prio(CH)->writeHead);
	TEST_ASSERT_EQUAL_UINT(6, prio(CH)->statusCheckHead);
}

static void test_scheduler_does_not_issue_while_controller_busy(void)
{
	enqueue_default_req(REQ_CODE_WRITE);
	mock_nsc_set_controller_busy(chCtlReg[CH], 1);

	SchedulingNandReqPerCh(CH);

	TEST_ASSERT_EQUAL_size_t(0, mock_nsc_count_op(MOCK_NSC_OP_PROGRAM));
	TEST_ASSERT_EQUAL_UINT(WAY, prio(CH)->writeHead);
	TEST_ASSERT_EQUAL_UINT(DIE_STATE_IDLE, die(CH, WAY)->dieState);

	mock_nsc_set_controller_busy(chCtlReg[CH], 0);
	SchedulingNandReqPerCh(CH);
	TEST_ASSERT_EQUAL_size_t(1, mock_nsc_count_op(MOCK_NSC_OP_PROGRAM));
}

static void test_scheduler_skips_status_report_for_busy_way(void)
{
	enqueue_default_req(REQ_CODE_WRITE);
	SchedulingNandReqPerCh(CH);
	SchedulingNandReqPerCh(CH);
	TEST_ASSERT_EQUAL_UINT(WAY, prio(CH)->statusReportHead);

	mock_nsc_set_ready_busy(chCtlReg[CH], READY_BUSY_ALL_BUT_WAY);
	SchedulingNandReqPerCh(CH);
	TEST_ASSERT_EQUAL_UINT(WAY, prio(CH)->statusReportHead);
	TEST_ASSERT_EQUAL_UINT(1, notCompletedNandReqCnt);

	mock_nsc_set_ready_busy(chCtlReg[CH], 0xffffffffu);
	SchedulingNandReqPerCh(CH);
	TEST_ASSERT_EQUAL_UINT(0, notCompletedNandReqCnt);
}

/* A pending status report (report done, request not complete) flips the die
 * back to OPT_CHECK; the scheduler moves it from the report list to the
 * statusCheck list and, in the same pass, re-issues the status check so the
 * way ends the pass on the report list again with a fresh report. */
static void test_scheduler_repolls_status_while_report_is_pending(void)
{
	enqueue_default_req(REQ_CODE_READ);
	mock_nsc_set_status_report(chCtlReg[CH], WAY, MOCK_NSC_STATUS_PENDING);

	SchedulingNandReqPerCh(CH); /* trigger issued -> statusCheck list */
	SchedulingNandReqPerCh(CH); /* status check -> report list */
	TEST_ASSERT_EQUAL_UINT(WAY, prio(CH)->statusReportHead);
	TEST_ASSERT_EQUAL_size_t(1, mock_nsc_count_op(MOCK_NSC_OP_STATUS_CHECK));

	SchedulingNandReqPerCh(CH); /* pending -> statusCheck list -> re-check -> report list */
	TEST_ASSERT_EQUAL_UINT(WAY, prio(CH)->statusReportHead);
	TEST_ASSERT_EQUAL_UINT(WAY_NONE, prio(CH)->statusCheckHead);
	TEST_ASSERT_EQUAL_UINT(REQ_STATUS_CHECK_OPT_REPORT, die(CH, WAY)->reqStatusCheckOpt);
	TEST_ASSERT_EQUAL_size_t(2, mock_nsc_count_op(MOCK_NSC_OP_STATUS_CHECK));
	TEST_ASSERT_EQUAL_UINT(REQ_CODE_READ, reqPoolPtr->reqPool[nandReqQ[CH][WAY].headReq].reqCode);
	TEST_ASSERT_EQUAL_UINT(1, notCompletedNandReqCnt);

	SchedulingNandReqPerCh(CH);
	TEST_ASSERT_EQUAL_size_t(3, mock_nsc_count_op(MOCK_NSC_OP_STATUS_CHECK));
	TEST_ASSERT_EQUAL_size_t(0, mock_nsc_count_op(MOCK_NSC_OP_READ_TRANSFER));

	mock_nsc_set_status_report(chCtlReg[CH], WAY, MOCK_NSC_STATUS_PASS);
	run_scheduler_until_idle();
	TEST_ASSERT_EQUAL_size_t(1, mock_nsc_count_op(MOCK_NSC_OP_READ_TRANSFER));
}

static void test_scheduler_completes_pending_write_once_report_passes(void)
{
	enqueue_default_req(REQ_CODE_WRITE);
	mock_nsc_set_status_report(chCtlReg[CH], WAY, MOCK_NSC_STATUS_PENDING);

	SchedulingNandReqPerCh(CH);
	SchedulingNandReqPerCh(CH);
	SchedulingNandReqPerCh(CH);
	TEST_ASSERT_EQUAL_UINT(WAY, prio(CH)->statusReportHead);
	TEST_ASSERT_EQUAL_UINT(1, notCompletedNandReqCnt);

	mock_nsc_set_status_report(chCtlReg[CH], WAY, MOCK_NSC_STATUS_PASS);
	SchedulingNandReqPerCh(CH); /* stale pending report consumed, fresh check issued */
	TEST_ASSERT_EQUAL_UINT(1, notCompletedNandReqCnt);
	SchedulingNandReqPerCh(CH); /* passing report completes the write */
	TEST_ASSERT_EQUAL_UINT(0, notCompletedNandReqCnt);
	TEST_ASSERT_EQUAL_UINT(WAY, prio(CH)->idleTail);
	TEST_ASSERT_EQUAL_UINT(DIE_STATE_IDLE, die(CH, WAY)->dieState);
}

static void test_scheduler_keeps_way_off_idle_list_while_queue_has_more_requests(void)
{
	enqueue_default_req(REQ_CODE_WRITE);
	enqueue_default_req(REQ_CODE_WRITE);

	SchedulingNandReqPerCh(CH);
	SchedulingNandReqPerCh(CH);
	SchedulingNandReqPerCh(CH); /* first write done, second goes straight to the write list */

	TEST_ASSERT_EQUAL_UINT(1, notCompletedNandReqCnt);
	TEST_ASSERT_EQUAL_UINT(WAY, prio(CH)->statusCheckHead);
	TEST_ASSERT_EQUAL_UINT(WAY + 1, die(CH, WAY - 1)->nextWay);
	TEST_ASSERT_EQUAL_UINT(WAY - 1, die(CH, WAY + 1)->prevWay);
	TEST_ASSERT_EQUAL_size_t(2, mock_nsc_count_op(MOCK_NSC_OP_PROGRAM));
}

static void test_scheduler_runs_read_as_trigger_then_transfer(void)
{
	enqueue_default_req(REQ_CODE_READ);

	run_scheduler_until_idle();

	TEST_ASSERT_EQUAL_size_t(1, mock_nsc_count_op(MOCK_NSC_OP_READ_TRIGGER));
	TEST_ASSERT_EQUAL_size_t(1, mock_nsc_count_op(MOCK_NSC_OP_STATUS_CHECK));
	TEST_ASSERT_EQUAL_size_t(1, mock_nsc_count_op(MOCK_NSC_OP_READ_TRANSFER));
	TEST_ASSERT_EQUAL_UINT(DIE_STATE_IDLE, die(CH, WAY)->dieState);
}

static void test_scheduler_read_returns_data_programmed_at_same_row(void)
{
	unsigned char *writeBuf = (unsigned char *)(uintptr_t)RESERVED_DATA_BUFFER_BASE_ADDR;
	unsigned char *readBuf = (unsigned char *)(uintptr_t)TEMPORARY_DATA_BUFFER_BASE_ADDR;
	unsigned int tag, i;

	for (i = 0; i < BYTES_PER_DATA_REGION_OF_SLICE; i++)
		writeBuf[i] = (unsigned char)(i * 7 + 1);
	memset(readBuf, 0, BYTES_PER_DATA_REGION_OF_SLICE);

	enqueue_default_req(REQ_CODE_WRITE);
	run_scheduler_until_idle();

	tag = enqueue_default_req(REQ_CODE_READ);
	reqPoolPtr->reqPool[tag].dataBufInfo.addr = TEMPORARY_DATA_BUFFER_BASE_ADDR;
	run_scheduler_until_idle();
	TEST_ASSERT_EQUAL_MEMORY(writeBuf, readBuf, BYTES_PER_DATA_REGION_OF_SLICE);

	/* a different page of the same block was never programmed: reads as erased */
	memset(readBuf, 0xA5, BYTES_PER_DATA_REGION_OF_SLICE);
	tag = enqueue_nand_req(CH, WAY, REQ_CODE_READ, TEST_BLOCK, TEST_PAGE + 1);
	reqPoolPtr->reqPool[tag].dataBufInfo.addr = TEMPORARY_DATA_BUFFER_BASE_ADDR;
	run_scheduler_until_idle();
	TEST_ASSERT_EACH_EQUAL_HEX8(0xFF, readBuf, BYTES_PER_DATA_REGION_OF_SLICE);
}

static void test_scheduler_retries_failed_read_until_retry_limit_then_marks_block_bad(void)
{
	unsigned int dieNo = Pcw2VdieTranslation(CH, WAY);

	enqueue_default_req(REQ_CODE_READ);
	mock_nsc_set_transfer_result(chCtlReg[CH], WAY, 1, ERRINFO0_CRC_FAIL, MOCK_NSC_ERRINFO1_CLEAN);

	run_scheduler_until_idle();

	TEST_ASSERT_EQUAL_size_t(RETRY_LIMIT + 1, mock_nsc_count_op(MOCK_NSC_OP_READ_TRIGGER));
	TEST_ASSERT_EQUAL_size_t(RETRY_LIMIT + 1, mock_nsc_count_op(MOCK_NSC_OP_READ_TRANSFER));
	TEST_ASSERT_EQUAL_UINT(BLOCK_STATE_BAD, phyBlockMapPtr->phyBlock[dieNo][TEST_BLOCK].bad);
	TEST_ASSERT_EQUAL_INT(RETRY_LIMIT, retryLimitTablePtr->retryLimit[CH][WAY]);
	TEST_ASSERT_EQUAL_UINT(WAY, prio(CH)->idleTail);
}

static void test_scheduler_recovers_read_that_succeeds_on_retry(void)
{
	unsigned int dieNo = Pcw2VdieTranslation(CH, WAY);
	unsigned int i;

	enqueue_default_req(REQ_CODE_READ);
	mock_nsc_set_transfer_result(chCtlReg[CH], WAY, 1, ERRINFO0_CRC_FAIL, MOCK_NSC_ERRINFO1_CLEAN);

	for (i = 0; i < MAX_SCHEDULER_ITERATIONS && mock_nsc_count_op(MOCK_NSC_OP_READ_TRANSFER) < 1; i++)
		SchedulingNandReq();
	mock_nsc_set_transfer_result(chCtlReg[CH], WAY, 1, MOCK_NSC_ERRINFO0_CLEAN, MOCK_NSC_ERRINFO1_CLEAN);
	run_scheduler_until_idle();

	TEST_ASSERT_EQUAL_size_t(2, mock_nsc_count_op(MOCK_NSC_OP_READ_TRIGGER));
	TEST_ASSERT_EQUAL_size_t(2, mock_nsc_count_op(MOCK_NSC_OP_READ_TRANSFER));
	TEST_ASSERT_NOT_EQUAL(BLOCK_STATE_BAD, phyBlockMapPtr->phyBlock[dieNo][TEST_BLOCK].bad);
	TEST_ASSERT_EQUAL_INT(RETRY_LIMIT, retryLimitTablePtr->retryLimit[CH][WAY]);
}

static void test_scheduler_failed_program_marks_block_bad_without_retry(void)
{
	unsigned int dieNo = Pcw2VdieTranslation(CH, WAY);

	enqueue_default_req(REQ_CODE_WRITE);
	mock_nsc_set_status_report(chCtlReg[CH], WAY, MOCK_NSC_STATUS_FAIL);

	run_scheduler_until_idle();

	TEST_ASSERT_EQUAL_size_t(1, mock_nsc_count_op(MOCK_NSC_OP_PROGRAM));
	TEST_ASSERT_EQUAL_UINT(BLOCK_STATE_BAD, phyBlockMapPtr->phyBlock[dieNo][TEST_BLOCK].bad);
}

static void test_scheduling_nand_req_serves_every_channel(void)
{
	enqueue_nand_req(0, 1, REQ_CODE_ERASE, TEST_BLOCK, 0);
	enqueue_nand_req(1, 2, REQ_CODE_ERASE, TEST_BLOCK, 0);

	SchedulingNandReq();

	TEST_ASSERT_EQUAL_size_t(2, mock_nsc_count_op(MOCK_NSC_OP_ERASE));
	TEST_ASSERT_EQUAL_PTR(chCtlReg[0], nth_op(MOCK_NSC_OP_ERASE, 0)->dev);
	TEST_ASSERT_EQUAL_PTR(chCtlReg[1], nth_op(MOCK_NSC_OP_ERASE, 1)->dev);
	TEST_ASSERT_EQUAL_UINT(DIE_STATE_EXE, die(0, 1)->dieState);
	TEST_ASSERT_EQUAL_UINT(DIE_STATE_EXE, die(1, 2)->dieState);
}

static void test_sync_all_low_level_req_done_drains_nand_queues(void)
{
	enqueue_nand_req(0, 0, REQ_CODE_WRITE, TEST_BLOCK, 0);
	enqueue_nand_req(1, 7, REQ_CODE_READ, TEST_BLOCK, 0);

	SyncAllLowLevelReqDone();

	TEST_ASSERT_EQUAL_UINT(0, notCompletedNandReqCnt);
	TEST_ASSERT_EQUAL_size_t(1, mock_nsc_count_op(MOCK_NSC_OP_PROGRAM));
	TEST_ASSERT_EQUAL_size_t(1, mock_nsc_count_op(MOCK_NSC_OP_READ_TRANSFER));
	TEST_ASSERT_EQUAL_UINT(DIE_STATE_IDLE, die(0, 0)->dieState);
	TEST_ASSERT_EQUAL_UINT(DIE_STATE_IDLE, die(1, 7)->dieState);
}

static void test_sync_avail_free_req_returns_immediately_when_free_slots_exist(void)
{
	SyncAvailFreeReq();
	TEST_ASSERT_EQUAL_size_t(0, mock_nsc_call_count());
}

static void test_sync_avail_free_req_completes_nand_work_when_pool_is_empty(void)
{
	unsigned int tag = enqueue_default_req(REQ_CODE_WRITE);
	unsigned int other;

	/* drain the free pool so the sync has to wait for the NAND write to return a slot */
	while (freeReqQ.headReq != REQ_SLOT_TAG_NONE)
		other = GetFromFreeReqQ();
	(void)other;

	SyncAvailFreeReq();

	TEST_ASSERT_EQUAL_UINT(tag, freeReqQ.headReq);
	TEST_ASSERT_EQUAL_UINT(0, notCompletedNandReqCnt);
}

int main(void)
{
	boot_once();
	UNITY_BEGIN();
	RUN_TEST(test_all_dies_idle_after_boot);
	RUN_TEST(test_idle_list_is_doubly_linked_in_way_order_after_boot);

	RUN_TEST(test_every_way_list_supports_put_and_selective_get);
	RUN_TEST(test_way_lists_are_independent_per_channel);
	RUN_TEST(test_put_to_way_priority_table_routes_by_req_code);
	RUN_TEST(test_put_to_way_priority_table_asserts_on_unknown_req_code);

	RUN_TEST(test_row_addr_phy_total_block_space_lun0_and_lun1);
	RUN_TEST(test_row_addr_phy_main_block_space_follows_remap);
	RUN_TEST(test_row_addr_vsa_uses_slc_lsb_page_translation);
	RUN_TEST(test_row_addr_asserts_on_unknown_nand_addr_option);
	RUN_TEST(test_data_and_spare_buf_addr_for_each_nand_buffer_format);
	RUN_TEST(test_data_buf_addr_for_nvme_dma_adds_block_offset);
	RUN_TEST(test_buf_addr_asserts_on_slice_req_type);

	RUN_TEST(test_issue_read_triggers_page_read_and_arms_status_check);
	RUN_TEST(test_issue_read_transfer_with_ecc_fills_error_info_and_completion);
	RUN_TEST(test_issue_read_transfer_without_ecc_uses_raw_transfer);
	RUN_TEST(test_issue_write_programs_page_with_data_and_spare);
	RUN_TEST(test_issue_erase_erases_block);
	RUN_TEST(test_issue_reset_and_set_feature_are_synchronous);
	RUN_TEST(test_issue_asserts_on_unknown_req_code);

	RUN_TEST(test_check_status_opt_check_issues_status_check_and_moves_to_report);
	RUN_TEST(test_check_status_report_pass_returns_done_and_clears_opt);
	RUN_TEST(test_check_status_report_fail_returns_fail);
	RUN_TEST(test_check_status_report_pending_reissues_status_check);
	RUN_TEST(test_check_status_report_not_yet_written_keeps_waiting);
	RUN_TEST(test_check_status_completion_flag_clear_is_running);
	RUN_TEST(test_check_status_completion_with_clean_ecc_is_done);
	RUN_TEST(test_check_status_completion_with_uncorrectable_ecc_is_fail);
	RUN_TEST(test_check_status_completion_with_many_bit_errors_is_warning);
	RUN_TEST(test_check_status_completion_ignores_ecc_when_ecc_off);
	RUN_TEST(test_check_status_opt_none_follows_ready_busy);
	RUN_TEST(test_check_status_asserts_on_unknown_option);
	RUN_TEST(test_ecc_error_info_classification);

	RUN_TEST(test_execute_on_idle_die_issues_request_and_enters_exe);
	RUN_TEST(test_execute_running_on_exe_die_does_nothing);
	RUN_TEST(test_execute_done_write_completes_request_and_resets_retry_limit);
	RUN_TEST(test_execute_done_read_trigger_converts_to_read_transfer);
	RUN_TEST(test_execute_failed_read_transfer_retries_as_read_trigger);
	RUN_TEST(test_execute_failed_read_trigger_retries_and_keeps_code);
	RUN_TEST(test_execute_failed_read_with_exhausted_retries_marks_grown_bad_block);
	RUN_TEST(test_execute_failed_write_is_not_retried_and_marks_grown_bad_block);
	RUN_TEST(test_execute_failed_erase_in_lun1_marks_correct_block);
	RUN_TEST(test_execute_failed_raw_read_into_addr_buffer_writes_pseudo_bad_mark);
	RUN_TEST(test_execute_failed_read_with_ecc_on_leaves_data_buffer_alone);
	RUN_TEST(test_execute_warning_completes_request_and_marks_grown_bad_block);
	RUN_TEST(test_execute_asserts_on_unknown_status);

	RUN_TEST(test_scheduler_leaves_idle_channel_untouched);
	RUN_TEST(test_scheduler_moves_write_through_status_check_and_report_lists);
	RUN_TEST(test_scheduler_issues_lists_in_priority_order);
	RUN_TEST(test_scheduler_takes_ways_off_idle_list_only_when_they_have_work);
	RUN_TEST(test_scheduler_does_not_issue_while_controller_busy);
	RUN_TEST(test_scheduler_skips_status_report_for_busy_way);
	RUN_TEST(test_scheduler_issues_one_request_per_list_per_pass);
	RUN_TEST(test_scheduler_repolls_status_while_report_is_pending);
	RUN_TEST(test_scheduler_completes_pending_write_once_report_passes);
	RUN_TEST(test_scheduler_keeps_way_off_idle_list_while_queue_has_more_requests);
	RUN_TEST(test_scheduler_runs_read_as_trigger_then_transfer);
	RUN_TEST(test_scheduler_read_returns_data_programmed_at_same_row);
	RUN_TEST(test_scheduler_retries_failed_read_until_retry_limit_then_marks_block_bad);
	RUN_TEST(test_scheduler_recovers_read_that_succeeds_on_retry);
	RUN_TEST(test_scheduler_failed_program_marks_block_bad_without_retry);
	RUN_TEST(test_scheduling_nand_req_serves_every_channel);
	RUN_TEST(test_sync_all_low_level_req_done_drains_nand_queues);
	RUN_TEST(test_sync_avail_free_req_returns_immediately_when_free_slots_exist);
	RUN_TEST(test_sync_avail_free_req_completes_nand_work_when_pool_is_empty);
	return UNITY_END();
}
