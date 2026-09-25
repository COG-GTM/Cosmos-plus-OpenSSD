/* Unit tests for request_schedule.c: die-state machine, status/ECC fail and
 * retry paths, Sync* helpers. List helpers and address generators live in
 * test_request_schedule_lists.c. */
#include "unity.h"

#include "fw_test.h"
#include "memory_map.h"

/* request_schedule.h declares this as dieStatusTablePtr (typo); the
 * definition in request_schedule.c is dieStateTablePtr. */
extern P_DIE_STATE_TABLE dieStateTablePtr;

#define TEST_CH 0
#define TEST_BLOCK 5
#define TEST_PAGE 3

/* Status report: report-done bit set, but status not yet "complete". */
#define STATUS_REPORT_PENDING 1U
/* Status report word that never reports done. */
#define STATUS_REPORT_SILENT 0U
/* errorInfo0 with CRC + spare chunk valid and worst chunk error count above
 * BIT_ERROR_THRESHOLD_PER_CHUNK. */
#define ERROR_INFO0_WARNING (MOCK_NSC_ERROR_INFO0_PASS | ((BIT_ERROR_THRESHOLD_PER_CHUNK + 1) << 16))
#define ERROR_INFO0_CRC_INVALID 0x01000000U
#define ERROR_INFO1_PAGE_CHUNK_INVALID 0xFFFFFFFEU

static unsigned int busy_after_cmd;
static unsigned int busy_after_cmd_count;
static unsigned int pass_after_status_checks;

void setUp(void)
{
	fw_test_reset();
	fw_test_init_ftl();
	mock_nsc_reset();
	busy_after_cmd = MOCK_NSC_MAX_CMD;
	busy_after_cmd_count = 0;
	pass_after_status_checks = 0;
}

void tearDown(void) {}

/* ------------------------------------------------------------------------ */
/* helpers                                                                   */
/* ------------------------------------------------------------------------ */

static SSD_REQ_FORMAT *req(unsigned int reqSlotTag)
{
	return &reqPoolPtr->reqPool[reqSlotTag];
}

static unsigned int alloc_phy_req(unsigned int reqCode, unsigned int wayNo, unsigned int blockNo, unsigned int pageNo)
{
	unsigned int reqSlotTag = GetFromFreeReqQ();
	SSD_REQ_FORMAT *r = req(reqSlotTag);

	r->reqType = REQ_TYPE_NAND;
	r->reqCode = reqCode;
	r->reqOpt.nandAddr = REQ_OPT_NAND_ADDR_PHY_ORG;
	r->reqOpt.dataBufFormat = REQ_OPT_DATA_BUF_NONE;
	r->reqOpt.nandEcc = REQ_OPT_NAND_ECC_ON;
	r->reqOpt.nandEccWarning = REQ_OPT_NAND_ECC_WARNING_OFF;
	r->reqOpt.rowAddrDependencyCheck = REQ_OPT_ROW_ADDR_DEPENDENCY_NONE;
	r->reqOpt.blockSpace = REQ_OPT_BLOCK_SPACE_TOTAL;
	r->nandInfo.physicalCh = TEST_CH;
	r->nandInfo.physicalWay = wayNo;
	r->nandInfo.physicalBlock = blockNo;
	r->nandInfo.physicalPage = pageNo;
	r->prevBlockingReq = REQ_SLOT_TAG_NONE;
	r->nextBlockingReq = REQ_SLOT_TAG_NONE;
	return reqSlotTag;
}

static unsigned int alloc_vsa_req(unsigned int reqCode, unsigned int dieNo, unsigned int blockNo, unsigned int pageNo)
{
	unsigned int reqSlotTag = GetFromFreeReqQ();
	SSD_REQ_FORMAT *r = req(reqSlotTag);

	r->reqType = REQ_TYPE_NAND;
	r->reqCode = reqCode;
	r->reqOpt.nandAddr = REQ_OPT_NAND_ADDR_VSA;
	r->reqOpt.dataBufFormat = REQ_OPT_DATA_BUF_NONE;
	r->reqOpt.nandEcc = REQ_OPT_NAND_ECC_ON;
	r->reqOpt.nandEccWarning = REQ_OPT_NAND_ECC_WARNING_OFF;
	r->reqOpt.rowAddrDependencyCheck = REQ_OPT_ROW_ADDR_DEPENDENCY_CHECK;
	r->reqOpt.blockSpace = REQ_OPT_BLOCK_SPACE_MAIN;
	r->nandInfo.virtualSliceAddr = Vorg2VsaTranslation(dieNo, blockNo, pageNo);
	r->nandInfo.programmedPageCnt = 0;
	r->prevBlockingReq = REQ_SLOT_TAG_NONE;
	r->nextBlockingReq = REQ_SLOT_TAG_NONE;
	return reqSlotTag;
}

static unsigned int submit_phy_req(unsigned int reqCode, unsigned int wayNo, unsigned int blockNo, unsigned int pageNo)
{
	unsigned int reqSlotTag = alloc_phy_req(reqCode, wayNo, blockNo, pageNo);

	SelectLowLevelReqQ(reqSlotTag);
	return reqSlotTag;
}

static void busy_hook(const mock_nsc_call_t *call)
{
	if (call->cmd == busy_after_cmd) {
		busy_after_cmd_count++;
		mock_nsc_set_controller_busy(1);
	}
}

static void pass_after_status_checks_hook(const mock_nsc_call_t *call)
{
	if (call->cmd != V2FCommand_StatusCheck)
		return;
	/* the mock wrote the report before calling us: switch after N-1 so the
	 * N-th status check is the first one that reports PASS */
	if (mock_nsc_count_cmd(V2FCommand_StatusCheck) + 1 >= pass_after_status_checks)
		mock_nsc_set_status_report(MOCK_NSC_STATUS_REPORT_PASS);
}

static unsigned int phy_block_is_bad(unsigned int wayNo, unsigned int blockNo)
{
	return phyBlockMapPtr->phyBlock[Pcw2VdieTranslation(TEST_CH, wayNo)][blockNo].bad == BLOCK_STATE_BAD;
}

/* ------------------------------------------------------------------------ */
/* smoke                                                                     */
/* ------------------------------------------------------------------------ */

static void test_smoke_scheduling_with_no_requests_issues_nothing(void)
{
	SchedulingNandReq();

	TEST_ASSERT_EQUAL_UINT(0, notCompletedNandReqCnt);
	TEST_ASSERT_EQUAL_UINT(0, mock_nsc_call_count());
}

static void test_smoke_erase_request_is_issued_and_completed(void)
{
	submit_phy_req(REQ_CODE_ERASE, 0, 1, 0);
	SyncAllLowLevelReqDone();

	TEST_ASSERT_EQUAL_UINT(1, mock_nsc_count_cmd(V2FCommand_BlockErase));
	TEST_ASSERT_EQUAL_UINT(0, notCompletedNandReqCnt);
}

/* ------------------------------------------------------------------------ */
/* happy-path state machine                                                  */
/* ------------------------------------------------------------------------ */

static void test_write_walks_idle_write_statuscheck_statusreport_and_completes(void)
{
	unsigned int reqSlotTag = submit_phy_req(REQ_CODE_WRITE, 0, TEST_BLOCK, TEST_PAGE);

	TEST_ASSERT_EQUAL_UINT(reqSlotTag, nandReqQ[TEST_CH][0].headReq);

	/* pass 1: idle -> write list -> issued -> status-check list */
	SchedulingNandReqPerCh(TEST_CH);
	TEST_ASSERT_EQUAL_UINT(1, mock_nsc_count_cmd(V2FCommand_ProgramPage));
	TEST_ASSERT_EQUAL_UINT(DIE_STATE_EXE, dieStateTablePtr->dieState[TEST_CH][0].dieState);
	TEST_ASSERT_EQUAL_UINT(0, wayPriorityTablePtr->wayPriority[TEST_CH].statusCheckHead);
	TEST_ASSERT_EQUAL_UINT(1, wayPriorityTablePtr->wayPriority[TEST_CH].idleHead);

	/* pass 2: status check issued -> status-report list */
	SchedulingNandReqPerCh(TEST_CH);
	TEST_ASSERT_EQUAL_UINT(1, mock_nsc_count_cmd(V2FCommand_StatusCheck));
	TEST_ASSERT_EQUAL_UINT(0, wayPriorityTablePtr->wayPriority[TEST_CH].statusReportHead);
	TEST_ASSERT_EQUAL_UINT(WAY_NONE, wayPriorityTablePtr->wayPriority[TEST_CH].statusCheckHead);
	TEST_ASSERT_EQUAL_UINT(REQ_STATUS_CHECK_OPT_REPORT, dieStateTablePtr->dieState[TEST_CH][0].reqStatusCheckOpt);

	/* pass 3: report says done -> request completes, way returns to idle */
	SchedulingNandReqPerCh(TEST_CH);
	TEST_ASSERT_EQUAL_UINT(0, notCompletedNandReqCnt);
	TEST_ASSERT_EQUAL_UINT(DIE_STATE_IDLE, dieStateTablePtr->dieState[TEST_CH][0].dieState);
	TEST_ASSERT_EQUAL_UINT(WAY_NONE, wayPriorityTablePtr->wayPriority[TEST_CH].statusReportHead);
	TEST_ASSERT_EQUAL_UINT(0, wayPriorityTablePtr->wayPriority[TEST_CH].idleTail);
	TEST_ASSERT_EQUAL_UINT(RETRY_LIMIT, retryLimitTablePtr->retryLimit[TEST_CH][0]);
}

static void test_read_becomes_read_transfer_and_completes(void)
{
	unsigned int reqSlotTag = submit_phy_req(REQ_CODE_READ, 0, TEST_BLOCK, TEST_PAGE);

	SchedulingNandReqPerCh(TEST_CH); /* trigger issued */
	TEST_ASSERT_EQUAL_UINT(1, mock_nsc_count_cmd(V2FCommand_ReadPageTrigger));
	TEST_ASSERT_EQUAL_UINT(0, wayPriorityTablePtr->wayPriority[TEST_CH].statusCheckHead);
	SchedulingNandReqPerCh(TEST_CH); /* status check */
	TEST_ASSERT_EQUAL_UINT(REQ_CODE_READ, req(reqSlotTag)->reqCode);
	TEST_ASSERT_EQUAL_UINT(0, wayPriorityTablePtr->wayPriority[TEST_CH].statusReportHead);

	/* report done -> READ_TRANSFER -> read-transfer list -> transfer issued in
	 * the same pass -> status-report list waiting on the completion flag */
	SchedulingNandReqPerCh(TEST_CH);
	TEST_ASSERT_EQUAL_UINT(REQ_CODE_READ_TRANSFER, req(reqSlotTag)->reqCode);
	TEST_ASSERT_EQUAL_UINT(1, mock_nsc_count_cmd(V2FCommand_ReadPageTransfer));
	TEST_ASSERT_EQUAL_UINT(WAY_NONE, wayPriorityTablePtr->wayPriority[TEST_CH].readTransferHead);
	TEST_ASSERT_EQUAL_UINT(0, wayPriorityTablePtr->wayPriority[TEST_CH].statusReportHead);
	TEST_ASSERT_EQUAL_UINT(REQ_STATUS_CHECK_OPT_COMPLETION_FLAG, dieStateTablePtr->dieState[TEST_CH][0].reqStatusCheckOpt);
	TEST_ASSERT_EQUAL_UINT(1, notCompletedNandReqCnt);

	SchedulingNandReqPerCh(TEST_CH); /* completion flag set -> done */
	TEST_ASSERT_EQUAL_UINT(0, notCompletedNandReqCnt);
	TEST_ASSERT_EQUAL_UINT(1, mock_nsc_count_cmd(V2FCommand_ReadPageTrigger));
}

static void test_read_with_ecc_off_uses_raw_transfer(void)
{
	unsigned int reqSlotTag = alloc_phy_req(REQ_CODE_READ, 0, TEST_BLOCK, TEST_PAGE);

	req(reqSlotTag)->reqOpt.nandEcc = REQ_OPT_NAND_ECC_OFF;
	req(reqSlotTag)->reqOpt.dataBufFormat = REQ_OPT_DATA_BUF_ADDR;
	req(reqSlotTag)->dataBufInfo.addr = RESERVED_DATA_BUFFER_BASE_ADDR;
	SelectLowLevelReqQ(reqSlotTag);
	SyncAllLowLevelReqDone();

	TEST_ASSERT_EQUAL_UINT(1, mock_nsc_count_cmd(V2FCommand_ReadPageTransferRaw));
	TEST_ASSERT_EQUAL_UINT(0, mock_nsc_count_cmd(V2FCommand_ReadPageTransfer));
	TEST_ASSERT_EQUAL_PTR(fw_ptr(RESERVED_DATA_BUFFER_BASE_ADDR), mock_nsc_last_call()->pageDataBuffer);
}

static void test_reset_and_set_feature_go_through_write_list_without_status_check(void)
{
	submit_phy_req(REQ_CODE_RESET, 0, 0, 0);
	submit_phy_req(REQ_CODE_SET_FEATURE, 1, 0, 0);

	SchedulingNandReqPerCh(TEST_CH);
	TEST_ASSERT_EQUAL_UINT(2, wayPriorityTablePtr->wayPriority[TEST_CH].idleHead);
	TEST_ASSERT_EQUAL_UINT(1, wayPriorityTablePtr->wayPriority[TEST_CH].writeHead);
	TEST_ASSERT_EQUAL_UINT(1, mock_nsc_count_cmd(V2FCommand_Reset));
	SchedulingNandReqPerCh(TEST_CH);
	TEST_ASSERT_EQUAL_UINT(1, mock_nsc_count_cmd(V2FCommand_SetFeatures));
	TEST_ASSERT_EQUAL_UINT(REQ_STATUS_CHECK_OPT_NONE, dieStateTablePtr->dieState[TEST_CH][0].reqStatusCheckOpt);
	TEST_ASSERT_EQUAL_UINT(REQ_STATUS_CHECK_OPT_NONE, dieStateTablePtr->dieState[TEST_CH][1].reqStatusCheckOpt);

	SyncAllLowLevelReqDone();
	TEST_ASSERT_EQUAL_UINT(0, mock_nsc_count_cmd(V2FCommand_StatusCheck));
	TEST_ASSERT_EQUAL_UINT(0, notCompletedNandReqCnt);
}

static void test_requests_on_all_ways_complete_and_idle_list_is_rebuilt(void)
{
	unsigned int wayNo;

	for (wayNo = 0; wayNo < USER_WAYS; wayNo++)
		submit_phy_req(REQ_CODE_WRITE, wayNo, TEST_BLOCK, wayNo);

	/* the first pass moves every way to the write list; each pass then issues
	 * exactly one program per channel because the per-list issue loops read
	 * nextWay after the way was moved to the status-check list */
	SchedulingNandReqPerCh(TEST_CH);
	TEST_ASSERT_EQUAL_UINT(WAY_NONE, wayPriorityTablePtr->wayPriority[TEST_CH].idleHead);
	TEST_ASSERT_EQUAL_UINT(WAY_NONE, wayPriorityTablePtr->wayPriority[TEST_CH].idleTail);
	TEST_ASSERT_EQUAL_UINT(1, mock_nsc_count_cmd(V2FCommand_ProgramPage));
	TEST_ASSERT_EQUAL_UINT(1, wayPriorityTablePtr->wayPriority[TEST_CH].writeHead);
	TEST_ASSERT_EQUAL_UINT(0, wayPriorityTablePtr->wayPriority[TEST_CH].statusCheckHead);
	for (wayNo = 1; wayNo < USER_WAYS; wayNo++) {
		SchedulingNandReqPerCh(TEST_CH);
		TEST_ASSERT_EQUAL_UINT(wayNo + 1, mock_nsc_count_cmd(V2FCommand_ProgramPage));
		TEST_ASSERT_EQUAL_INT(wayNo, mock_nsc_last_call()->way);
	}
	TEST_ASSERT_EQUAL_UINT(WAY_NONE, wayPriorityTablePtr->wayPriority[TEST_CH].writeHead);

	SyncAllLowLevelReqDone();
	TEST_ASSERT_EQUAL_UINT(0, notCompletedNandReqCnt);
	TEST_ASSERT_NOT_EQUAL(WAY_NONE, wayPriorityTablePtr->wayPriority[TEST_CH].idleHead);

	/* every way is back in the idle list exactly once */
	{
		unsigned int seen = 0, count = 0;

		wayNo = wayPriorityTablePtr->wayPriority[TEST_CH].idleHead;
		while (wayNo != WAY_NONE) {
			seen |= 1U << wayNo;
			count++;
			wayNo = dieStateTablePtr->dieState[TEST_CH][wayNo].nextWay;
		}
		TEST_ASSERT_EQUAL_UINT(USER_WAYS, count);
		TEST_ASSERT_EQUAL_HEX32((1U << USER_WAYS) - 1, seen);
	}
}

static void test_queued_requests_on_same_way_run_in_order(void)
{
	unsigned int first = submit_phy_req(REQ_CODE_ERASE, 0, 10, 0);
	unsigned int second = submit_phy_req(REQ_CODE_WRITE, 0, 10, 0);
	const mock_nsc_call_t *call;

	TEST_ASSERT_EQUAL_UINT(first, nandReqQ[TEST_CH][0].headReq);
	TEST_ASSERT_EQUAL_UINT(second, nandReqQ[TEST_CH][0].tailReq);

	SyncAllLowLevelReqDone();

	TEST_ASSERT_EQUAL_UINT(0, notCompletedNandReqCnt);
	TEST_ASSERT_EQUAL_UINT(1, mock_nsc_count_cmd(V2FCommand_ProgramPage));
	TEST_ASSERT_EQUAL_UINT(1, mock_nsc_count_cmd(V2FCommand_BlockErase));

	/* every op before the program must belong to the erase (erase itself or
	 * its status checks); the program is the last non-status command */
	{
		unsigned int i, eraseIndex = 0, programIndex = 0;
		unsigned int seenErase = 0, seenProgram = 0;

		for (i = 0; i < mock_nsc_call_count(); i++) {
			call = mock_nsc_call_at(i);
			TEST_ASSERT_NOT_NULL(call);
			if (call->cmd == V2FCommand_BlockErase) {
				eraseIndex = i;
				seenErase = 1;
			} else if (call->cmd == V2FCommand_ProgramPage) {
				programIndex = i;
				seenProgram = 1;
			} else {
				TEST_ASSERT_EQUAL_UINT(V2FCommand_StatusCheck, call->cmd);
			}
		}
		TEST_ASSERT_TRUE(seenErase && seenProgram);
		TEST_ASSERT_EQUAL_UINT(0, eraseIndex);
		TEST_ASSERT_TRUE(programIndex > eraseIndex);
	}
}

/* ------------------------------------------------------------------------ */
/* ready/busy and controller-busy back-off                                   */
/* ------------------------------------------------------------------------ */

static void test_way_not_ready_keeps_request_in_status_report_list(void)
{
	submit_phy_req(REQ_CODE_WRITE, 0, TEST_BLOCK, TEST_PAGE);

	SchedulingNandReqPerCh(TEST_CH); /* issue */
	SchedulingNandReqPerCh(TEST_CH); /* status check -> status-report list */
	TEST_ASSERT_EQUAL_UINT(0, wayPriorityTablePtr->wayPriority[TEST_CH].statusReportHead);

	mock_nsc_set_ready_busy(0);
	SchedulingNandReqPerCh(TEST_CH);
	SchedulingNandReqPerCh(TEST_CH);
	TEST_ASSERT_EQUAL_UINT(0, wayPriorityTablePtr->wayPriority[TEST_CH].statusReportHead);
	TEST_ASSERT_EQUAL_UINT(1, notCompletedNandReqCnt);

	mock_nsc_set_ready_busy(MOCK_NSC_ALL_WAYS_READY);
	SyncAllLowLevelReqDone();
	TEST_ASSERT_EQUAL_UINT(0, notCompletedNandReqCnt);
}

static void test_way_not_ready_keeps_request_in_status_check_list(void)
{
	submit_phy_req(REQ_CODE_WRITE, 0, TEST_BLOCK, TEST_PAGE);

	SchedulingNandReqPerCh(TEST_CH); /* issue -> status-check list */
	mock_nsc_set_ready_busy(0);
	SchedulingNandReqPerCh(TEST_CH);
	TEST_ASSERT_EQUAL_UINT(0, wayPriorityTablePtr->wayPriority[TEST_CH].statusCheckHead);
	TEST_ASSERT_EQUAL_UINT(0, mock_nsc_count_cmd(V2FCommand_StatusCheck));

	mock_nsc_set_ready_busy(MOCK_NSC_ALL_WAYS_READY);
	SyncAllLowLevelReqDone();
	TEST_ASSERT_EQUAL_UINT(0, notCompletedNandReqCnt);
}

static void test_status_report_not_done_keeps_way_in_status_report_list(void)
{
	mock_nsc_set_status_report(STATUS_REPORT_SILENT);
	submit_phy_req(REQ_CODE_WRITE, 0, TEST_BLOCK, TEST_PAGE);

	SchedulingNandReqPerCh(TEST_CH); /* issue */
	SchedulingNandReqPerCh(TEST_CH); /* status check */
	SchedulingNandReqPerCh(TEST_CH); /* report not done: wait */
	SchedulingNandReqPerCh(TEST_CH);
	TEST_ASSERT_EQUAL_UINT(0, wayPriorityTablePtr->wayPriority[TEST_CH].statusReportHead);
	TEST_ASSERT_EQUAL_UINT(REQ_STATUS_CHECK_OPT_REPORT, dieStateTablePtr->dieState[TEST_CH][0].reqStatusCheckOpt);
	TEST_ASSERT_EQUAL_UINT(1, mock_nsc_count_cmd(V2FCommand_StatusCheck));

	/* the NAND controller eventually writes the report word asynchronously */
	statusReportTablePtr->statusReport[TEST_CH][0] = MOCK_NSC_STATUS_REPORT_PASS;
	SyncAllLowLevelReqDone();
	TEST_ASSERT_EQUAL_UINT(1, mock_nsc_count_cmd(V2FCommand_StatusCheck));
	TEST_ASSERT_EQUAL_UINT(0, notCompletedNandReqCnt);
}

static void test_status_report_done_but_not_complete_reissues_status_check(void)
{
	mock_nsc_set_status_report(STATUS_REPORT_PENDING);
	pass_after_status_checks = 3;
	mock_nsc_set_hook(pass_after_status_checks_hook);
	submit_phy_req(REQ_CODE_ERASE, 0, TEST_BLOCK, 0);

	SchedulingNandReqPerCh(TEST_CH); /* issue */
	SchedulingNandReqPerCh(TEST_CH); /* status check #1 */
	TEST_ASSERT_EQUAL_UINT(1, mock_nsc_count_cmd(V2FCommand_StatusCheck));
	TEST_ASSERT_EQUAL_UINT(0, wayPriorityTablePtr->wayPriority[TEST_CH].statusReportHead);
	/* pending -> moved to status-check list -> status check #2 in same pass */
	SchedulingNandReqPerCh(TEST_CH);
	TEST_ASSERT_EQUAL_UINT(2, mock_nsc_count_cmd(V2FCommand_StatusCheck));
	TEST_ASSERT_EQUAL_UINT(0, wayPriorityTablePtr->wayPriority[TEST_CH].statusReportHead);
	TEST_ASSERT_EQUAL_UINT(1, notCompletedNandReqCnt);

	SyncAllLowLevelReqDone();
	TEST_ASSERT_EQUAL_UINT(0, notCompletedNandReqCnt);
	TEST_ASSERT_EQUAL_UINT(3, mock_nsc_count_cmd(V2FCommand_StatusCheck));
	TEST_ASSERT_EQUAL_UINT(1, mock_nsc_count_cmd(V2FCommand_BlockErase));
}

static void run_controller_busy_after(unsigned int cmd, unsigned int reqCode)
{
	busy_after_cmd = cmd;
	mock_nsc_set_hook(busy_hook);
	submit_phy_req(reqCode, 0, TEST_BLOCK, 0);
	submit_phy_req(reqCode, 1, TEST_BLOCK, 0);
}

static void test_controller_busy_after_write_stops_issuing_further_ways(void)
{
	run_controller_busy_after(V2FCommand_ProgramPage, REQ_CODE_WRITE);

	SchedulingNandReqPerCh(TEST_CH);
	TEST_ASSERT_EQUAL_UINT(1, mock_nsc_count_cmd(V2FCommand_ProgramPage));
	TEST_ASSERT_EQUAL_UINT(1, wayPriorityTablePtr->wayPriority[TEST_CH].writeHead);
	TEST_ASSERT_EQUAL_UINT(0, wayPriorityTablePtr->wayPriority[TEST_CH].statusCheckHead);

	/* still busy: nothing more is issued */
	SchedulingNandReqPerCh(TEST_CH);
	TEST_ASSERT_EQUAL_UINT(1, mock_nsc_count_cmd(V2FCommand_ProgramPage));

	mock_nsc_set_hook(NULL);
	mock_nsc_set_controller_busy(0);
	SyncAllLowLevelReqDone();
	TEST_ASSERT_EQUAL_UINT(2, mock_nsc_count_cmd(V2FCommand_ProgramPage));
	TEST_ASSERT_EQUAL_UINT(0, notCompletedNandReqCnt);
}

static void test_controller_busy_after_erase_stops_issuing_further_ways(void)
{
	run_controller_busy_after(V2FCommand_BlockErase, REQ_CODE_ERASE);

	SchedulingNandReqPerCh(TEST_CH);
	TEST_ASSERT_EQUAL_UINT(1, mock_nsc_count_cmd(V2FCommand_BlockErase));
	TEST_ASSERT_EQUAL_UINT(1, wayPriorityTablePtr->wayPriority[TEST_CH].eraseHead);

	mock_nsc_set_hook(NULL);
	mock_nsc_set_controller_busy(0);
	SyncAllLowLevelReqDone();
	TEST_ASSERT_EQUAL_UINT(2, mock_nsc_count_cmd(V2FCommand_BlockErase));
}

static void test_controller_busy_after_read_trigger_stops_issuing_further_ways(void)
{
	run_controller_busy_after(V2FCommand_ReadPageTrigger, REQ_CODE_READ);

	SchedulingNandReqPerCh(TEST_CH);
	TEST_ASSERT_EQUAL_UINT(1, mock_nsc_count_cmd(V2FCommand_ReadPageTrigger));
	TEST_ASSERT_EQUAL_UINT(1, wayPriorityTablePtr->wayPriority[TEST_CH].readTriggerHead);

	mock_nsc_set_hook(NULL);
	mock_nsc_set_controller_busy(0);
	SyncAllLowLevelReqDone();
	TEST_ASSERT_EQUAL_UINT(2, mock_nsc_count_cmd(V2FCommand_ReadPageTrigger));
	TEST_ASSERT_EQUAL_UINT(2, mock_nsc_count_cmd(V2FCommand_ReadPageTransfer));
}

static void test_controller_busy_after_read_transfer_stops_issuing_further_ways(void)
{
	unsigned int passes;

	run_controller_busy_after(V2FCommand_ReadPageTransfer, REQ_CODE_READ);
	for (passes = 0; passes < 4 && mock_nsc_count_cmd(V2FCommand_ReadPageTransfer) == 0; passes++)
		SchedulingNandReqPerCh(TEST_CH);
	TEST_ASSERT_EQUAL_UINT(1, mock_nsc_count_cmd(V2FCommand_ReadPageTransfer));
	TEST_ASSERT_EQUAL_UINT(1, busy_after_cmd_count);
	TEST_ASSERT_EQUAL_INT(0, mock_nsc_last_call()->way);
	TEST_ASSERT_EQUAL_UINT(0, wayPriorityTablePtr->wayPriority[TEST_CH].statusReportTail);

	/* controller stays busy: way 0 still completes (status-report list is
	 * served before the busy check) but way 1's transfer is held back */
	SchedulingNandReqPerCh(TEST_CH);
	SchedulingNandReqPerCh(TEST_CH);
	TEST_ASSERT_EQUAL_UINT(1, mock_nsc_count_cmd(V2FCommand_ReadPageTransfer));
	TEST_ASSERT_EQUAL_UINT(1, notCompletedNandReqCnt);
	TEST_ASSERT_EQUAL_UINT(1, wayPriorityTablePtr->wayPriority[TEST_CH].readTransferHead);

	mock_nsc_set_hook(NULL);
	mock_nsc_set_controller_busy(0);
	SyncAllLowLevelReqDone();
	TEST_ASSERT_EQUAL_UINT(2, mock_nsc_count_cmd(V2FCommand_ReadPageTransfer));
}

static void test_controller_busy_after_status_check_stops_checking_further_ways(void)
{
	run_controller_busy_after(V2FCommand_StatusCheck, REQ_CODE_WRITE);

	SchedulingNandReqPerCh(TEST_CH); /* way 0 issued */
	TEST_ASSERT_EQUAL_UINT(1, mock_nsc_count_cmd(V2FCommand_ProgramPage));

	/* status check for way 0 -> busy -> return before the write list is served */
	SchedulingNandReqPerCh(TEST_CH);
	TEST_ASSERT_EQUAL_UINT(1, mock_nsc_count_cmd(V2FCommand_StatusCheck));
	TEST_ASSERT_EQUAL_UINT(1, mock_nsc_count_cmd(V2FCommand_ProgramPage));
	TEST_ASSERT_EQUAL_UINT(0, wayPriorityTablePtr->wayPriority[TEST_CH].statusReportHead);
	TEST_ASSERT_EQUAL_UINT(WAY_NONE, wayPriorityTablePtr->wayPriority[TEST_CH].statusCheckHead);
	TEST_ASSERT_EQUAL_UINT(1, wayPriorityTablePtr->wayPriority[TEST_CH].writeHead);

	mock_nsc_set_hook(NULL);
	mock_nsc_set_controller_busy(0);
	SyncAllLowLevelReqDone();
	TEST_ASSERT_EQUAL_UINT(2, mock_nsc_count_cmd(V2FCommand_StatusCheck));
	TEST_ASSERT_EQUAL_UINT(2, mock_nsc_count_cmd(V2FCommand_ProgramPage));
	TEST_ASSERT_EQUAL_UINT(0, notCompletedNandReqCnt);
}

static void test_controller_busy_with_idle_ways_only_skips_issue_block(void)
{
	submit_phy_req(REQ_CODE_WRITE, 0, TEST_BLOCK, 0);
	mock_nsc_set_controller_busy(1);

	SchedulingNandReqPerCh(TEST_CH);
	TEST_ASSERT_EQUAL_UINT(0, wayPriorityTablePtr->wayPriority[TEST_CH].writeHead);
	TEST_ASSERT_EQUAL_UINT(0, mock_nsc_call_count());

	mock_nsc_set_controller_busy(0);
	SyncAllLowLevelReqDone();
	TEST_ASSERT_EQUAL_UINT(0, notCompletedNandReqCnt);
}

/* ------------------------------------------------------------------------ */
/* status fail / ECC paths                                                   */
/* ------------------------------------------------------------------------ */

static void test_write_status_fail_marks_grown_bad_block_and_completes(void)
{
	mock_nsc_set_status_report(MOCK_NSC_STATUS_REPORT_FAIL);
	submit_phy_req(REQ_CODE_WRITE, 2, TEST_BLOCK, TEST_PAGE);

	SyncAllLowLevelReqDone();

	TEST_ASSERT_EQUAL_UINT(0, notCompletedNandReqCnt);
	TEST_ASSERT_EQUAL_UINT(1, mock_nsc_count_cmd(V2FCommand_ProgramPage));
	TEST_ASSERT_TRUE(phy_block_is_bad(2, TEST_BLOCK));
	TEST_ASSERT_EQUAL_UINT(BBT_INFO_GROWN_BAD_UPDATE_BOOKED,
			bbtInfoMapPtr->bbtInfo[Pcw2VdieTranslation(TEST_CH, 2)].grownBadUpdate);
	TEST_ASSERT_EQUAL_UINT(RETRY_LIMIT, retryLimitTablePtr->retryLimit[TEST_CH][2]);
	TEST_ASSERT_EQUAL_UINT(DIE_STATE_IDLE, dieStateTablePtr->dieState[TEST_CH][2].dieState);
}

static void test_erase_status_fail_in_lun1_marks_correct_block(void)
{
	unsigned int blockNo = TOTAL_BLOCKS_PER_LUN + 7;

	mock_nsc_set_status_report(MOCK_NSC_STATUS_REPORT_FAIL);
	submit_phy_req(REQ_CODE_ERASE, 0, blockNo, 0);

	SyncAllLowLevelReqDone();

	TEST_ASSERT_EQUAL_HEX32(LUN_1_BASE_ADDR + 7 * PAGES_PER_MLC_BLOCK, mock_nsc_call_at(0)->rowAddress);
	TEST_ASSERT_TRUE(phy_block_is_bad(0, blockNo));
	TEST_ASSERT_FALSE(phy_block_is_bad(0, 7));
}

static void test_read_trigger_fail_retries_up_to_retry_limit(void)
{
	mock_nsc_set_status_report(MOCK_NSC_STATUS_REPORT_FAIL);
	submit_phy_req(REQ_CODE_READ, 0, TEST_BLOCK, TEST_PAGE);

	/* first failure: retry budget decremented, request re-triggered */
	SchedulingNandReqPerCh(TEST_CH); /* trigger */
	SchedulingNandReqPerCh(TEST_CH); /* status check */
	SchedulingNandReqPerCh(TEST_CH); /* FAIL -> retry -> back to read-trigger list */
	TEST_ASSERT_EQUAL_UINT(RETRY_LIMIT - 1, retryLimitTablePtr->retryLimit[TEST_CH][0]);
	TEST_ASSERT_EQUAL_UINT(1, notCompletedNandReqCnt);
	TEST_ASSERT_EQUAL_UINT(2, mock_nsc_count_cmd(V2FCommand_ReadPageTrigger));

	SyncAllLowLevelReqDone();

	TEST_ASSERT_EQUAL_UINT(RETRY_LIMIT + 1, mock_nsc_count_cmd(V2FCommand_ReadPageTrigger));
	TEST_ASSERT_EQUAL_UINT(0, mock_nsc_count_cmd(V2FCommand_ReadPageTransfer));
	TEST_ASSERT_TRUE(phy_block_is_bad(0, TEST_BLOCK));
	TEST_ASSERT_EQUAL_UINT(RETRY_LIMIT, retryLimitTablePtr->retryLimit[TEST_CH][0]);
	TEST_ASSERT_EQUAL_UINT(0, notCompletedNandReqCnt);
}

static void test_read_trigger_fail_with_ecc_off_and_buf_addr_writes_bad_mark(void)
{
	unsigned int reqSlotTag = alloc_phy_req(REQ_CODE_READ, 0, TEST_BLOCK, TEST_PAGE);
	unsigned char *mark = fw_ptr(RESERVED_DATA_BUFFER_BASE_ADDR);

	*mark = 0xA5;
	mock_nsc_set_status_report(MOCK_NSC_STATUS_REPORT_FAIL);
	req(reqSlotTag)->reqOpt.nandEcc = REQ_OPT_NAND_ECC_OFF;
	req(reqSlotTag)->reqOpt.dataBufFormat = REQ_OPT_DATA_BUF_ADDR;
	req(reqSlotTag)->dataBufInfo.addr = RESERVED_DATA_BUFFER_BASE_ADDR;
	SelectLowLevelReqQ(reqSlotTag);

	SyncAllLowLevelReqDone();

	TEST_ASSERT_EQUAL_HEX8(PSEUDO_BAD_BLOCK_MARK, *mark);
	TEST_ASSERT_TRUE(phy_block_is_bad(0, TEST_BLOCK));
}

static void test_read_transfer_ecc_fail_retries_from_trigger_then_marks_bad(void)
{
	mock_nsc_set_error_info(ERROR_INFO0_CRC_INVALID, MOCK_NSC_ERROR_INFO1_PASS);
	submit_phy_req(REQ_CODE_READ, 0, TEST_BLOCK, TEST_PAGE);

	SchedulingNandReqPerCh(TEST_CH); /* trigger */
	SchedulingNandReqPerCh(TEST_CH); /* status check */
	SchedulingNandReqPerCh(TEST_CH); /* done -> READ_TRANSFER -> transfer issued */
	TEST_ASSERT_EQUAL_UINT(1, mock_nsc_count_cmd(V2FCommand_ReadPageTransfer));
	SchedulingNandReqPerCh(TEST_CH); /* ECC fail -> retry as READ -> re-triggered */
	TEST_ASSERT_EQUAL_UINT(REQ_CODE_READ, req(nandReqQ[TEST_CH][0].headReq)->reqCode);
	TEST_ASSERT_EQUAL_UINT(RETRY_LIMIT - 1, retryLimitTablePtr->retryLimit[TEST_CH][0]);
	TEST_ASSERT_EQUAL_UINT(2, mock_nsc_count_cmd(V2FCommand_ReadPageTrigger));
	TEST_ASSERT_EQUAL_UINT(1, notCompletedNandReqCnt);

	SyncAllLowLevelReqDone();

	TEST_ASSERT_EQUAL_UINT(RETRY_LIMIT + 1, mock_nsc_count_cmd(V2FCommand_ReadPageTrigger));
	TEST_ASSERT_EQUAL_UINT(RETRY_LIMIT + 1, mock_nsc_count_cmd(V2FCommand_ReadPageTransfer));
	TEST_ASSERT_TRUE(phy_block_is_bad(0, TEST_BLOCK));
	TEST_ASSERT_EQUAL_UINT(0, notCompletedNandReqCnt);
}

static void test_read_transfer_page_chunk_invalid_is_ecc_fail(void)
{
	mock_nsc_set_error_info(MOCK_NSC_ERROR_INFO0_PASS, ERROR_INFO1_PAGE_CHUNK_INVALID);
	submit_phy_req(REQ_CODE_READ, 0, TEST_BLOCK, TEST_PAGE);

	SyncAllLowLevelReqDone();

	TEST_ASSERT_EQUAL_UINT(RETRY_LIMIT + 1, mock_nsc_count_cmd(V2FCommand_ReadPageTransfer));
	TEST_ASSERT_TRUE(phy_block_is_bad(0, TEST_BLOCK));
}

static void test_read_transfer_ecc_warning_completes_and_marks_block_bad(void)
{
	unsigned int reqSlotTag = alloc_phy_req(REQ_CODE_READ, 0, TEST_BLOCK, TEST_PAGE);

	mock_nsc_set_error_info(ERROR_INFO0_WARNING, MOCK_NSC_ERROR_INFO1_PASS);
	req(reqSlotTag)->reqOpt.nandEccWarning = REQ_OPT_NAND_ECC_WARNING_ON;
	SelectLowLevelReqQ(reqSlotTag);

	SyncAllLowLevelReqDone();

	TEST_ASSERT_EQUAL_UINT(1, mock_nsc_count_cmd(V2FCommand_ReadPageTrigger));
	TEST_ASSERT_EQUAL_UINT(1, mock_nsc_count_cmd(V2FCommand_ReadPageTransfer));
	TEST_ASSERT_TRUE(phy_block_is_bad(0, TEST_BLOCK));
	TEST_ASSERT_EQUAL_UINT(0, notCompletedNandReqCnt);
}

static void test_ecc_warning_ignored_when_warning_option_off(void)
{
	mock_nsc_set_error_info(ERROR_INFO0_WARNING, MOCK_NSC_ERROR_INFO1_PASS);
	submit_phy_req(REQ_CODE_READ, 0, TEST_BLOCK, TEST_PAGE);

	SyncAllLowLevelReqDone();

	TEST_ASSERT_FALSE(phy_block_is_bad(0, TEST_BLOCK));
	TEST_ASSERT_EQUAL_UINT(0, notCompletedNandReqCnt);
}

static void test_check_ecc_error_info_classifies_error_words(void)
{
	unsigned int reqSlotTag = submit_phy_req(REQ_CODE_READ_TRANSFER, 0, TEST_BLOCK, TEST_PAGE);
	unsigned int *info = eccErrorInfoTablePtr->errorInfo[TEST_CH][0];

	info[0] = MOCK_NSC_ERROR_INFO0_PASS;
	info[1] = MOCK_NSC_ERROR_INFO1_PASS;
	TEST_ASSERT_EQUAL_UINT(ERROR_INFO_PASS, CheckEccErrorInfo(TEST_CH, 0));

	info[0] = ERROR_INFO0_WARNING;
	TEST_ASSERT_EQUAL_UINT(ERROR_INFO_PASS, CheckEccErrorInfo(TEST_CH, 0));
	req(reqSlotTag)->reqOpt.nandEccWarning = REQ_OPT_NAND_ECC_WARNING_ON;
	TEST_ASSERT_EQUAL_UINT(ERROR_INFO_WARNING, CheckEccErrorInfo(TEST_CH, 0));

	info[0] = MOCK_NSC_ERROR_INFO0_PASS | (BIT_ERROR_THRESHOLD_PER_CHUNK << 16);
	TEST_ASSERT_EQUAL_UINT(ERROR_INFO_PASS, CheckEccErrorInfo(TEST_CH, 0));

	info[0] = ERROR_INFO0_CRC_INVALID;
	TEST_ASSERT_EQUAL_UINT(ERROR_INFO_FAIL, CheckEccErrorInfo(TEST_CH, 0));

	info[0] = 0x10000000U; /* CRC valid, spare chunk invalid */
	TEST_ASSERT_EQUAL_UINT(ERROR_INFO_FAIL, CheckEccErrorInfo(TEST_CH, 0));

	info[0] = MOCK_NSC_ERROR_INFO0_PASS;
	info[1] = ERROR_INFO1_PAGE_CHUNK_INVALID;
	TEST_ASSERT_EQUAL_UINT(ERROR_INFO_FAIL, CheckEccErrorInfo(TEST_CH, 0));
}

/* ------------------------------------------------------------------------ */
/* CheckReqStatus                                                            */
/* ------------------------------------------------------------------------ */

static void test_check_req_status_completion_flag_paths(void)
{
	unsigned int reqSlotTag = submit_phy_req(REQ_CODE_READ_TRANSFER, 0, TEST_BLOCK, TEST_PAGE);

	dieStateTablePtr->dieState[TEST_CH][0].reqStatusCheckOpt = REQ_STATUS_CHECK_OPT_COMPLETION_FLAG;
	completeFlagTablePtr->completeFlag[TEST_CH][0] = 0;
	TEST_ASSERT_EQUAL_UINT(REQ_STATUS_RUNNING, CheckReqStatus(TEST_CH, 0));

	completeFlagTablePtr->completeFlag[TEST_CH][0] = 1;
	eccErrorInfoTablePtr->errorInfo[TEST_CH][0][0] = MOCK_NSC_ERROR_INFO0_PASS;
	eccErrorInfoTablePtr->errorInfo[TEST_CH][0][1] = MOCK_NSC_ERROR_INFO1_PASS;
	TEST_ASSERT_EQUAL_UINT(REQ_STATUS_DONE, CheckReqStatus(TEST_CH, 0));

	eccErrorInfoTablePtr->errorInfo[TEST_CH][0][0] = ERROR_INFO0_CRC_INVALID;
	TEST_ASSERT_EQUAL_UINT(REQ_STATUS_FAIL, CheckReqStatus(TEST_CH, 0));

	eccErrorInfoTablePtr->errorInfo[TEST_CH][0][0] = ERROR_INFO0_WARNING;
	req(reqSlotTag)->reqOpt.nandEccWarning = REQ_OPT_NAND_ECC_WARNING_ON;
	TEST_ASSERT_EQUAL_UINT(REQ_STATUS_WARNING, CheckReqStatus(TEST_CH, 0));

	/* ECC off: error words are ignored */
	req(reqSlotTag)->reqOpt.nandEcc = REQ_OPT_NAND_ECC_OFF;
	eccErrorInfoTablePtr->errorInfo[TEST_CH][0][0] = ERROR_INFO0_CRC_INVALID;
	TEST_ASSERT_EQUAL_UINT(REQ_STATUS_DONE, CheckReqStatus(TEST_CH, 0));
}

static void test_check_req_status_report_paths(void)
{
	submit_phy_req(REQ_CODE_WRITE, 0, TEST_BLOCK, TEST_PAGE);

	dieStateTablePtr->dieState[TEST_CH][0].reqStatusCheckOpt = REQ_STATUS_CHECK_OPT_REPORT;

	statusReportTablePtr->statusReport[TEST_CH][0] = STATUS_REPORT_SILENT;
	TEST_ASSERT_EQUAL_UINT(REQ_STATUS_RUNNING, CheckReqStatus(TEST_CH, 0));
	TEST_ASSERT_EQUAL_UINT(REQ_STATUS_CHECK_OPT_REPORT, dieStateTablePtr->dieState[TEST_CH][0].reqStatusCheckOpt);

	statusReportTablePtr->statusReport[TEST_CH][0] = MOCK_NSC_STATUS_REPORT_FAIL;
	TEST_ASSERT_EQUAL_UINT(REQ_STATUS_FAIL, CheckReqStatus(TEST_CH, 0));

	statusReportTablePtr->statusReport[TEST_CH][0] = MOCK_NSC_STATUS_REPORT_PASS;
	TEST_ASSERT_EQUAL_UINT(REQ_STATUS_DONE, CheckReqStatus(TEST_CH, 0));
	TEST_ASSERT_EQUAL_UINT(REQ_STATUS_CHECK_OPT_NONE, dieStateTablePtr->dieState[TEST_CH][0].reqStatusCheckOpt);

	dieStateTablePtr->dieState[TEST_CH][0].reqStatusCheckOpt = REQ_STATUS_CHECK_OPT_REPORT;
	statusReportTablePtr->statusReport[TEST_CH][0] = STATUS_REPORT_PENDING;
	TEST_ASSERT_EQUAL_UINT(REQ_STATUS_RUNNING, CheckReqStatus(TEST_CH, 0));
	TEST_ASSERT_EQUAL_UINT(REQ_STATUS_CHECK_OPT_CHECK, dieStateTablePtr->dieState[TEST_CH][0].reqStatusCheckOpt);

	/* CHECK: issues a status-check command and moves to REPORT */
	TEST_ASSERT_EQUAL_UINT(REQ_STATUS_RUNNING, CheckReqStatus(TEST_CH, 0));
	TEST_ASSERT_EQUAL_UINT(1, mock_nsc_count_cmd(V2FCommand_StatusCheck));
	TEST_ASSERT_EQUAL_PTR(&statusReportTablePtr->statusReport[TEST_CH][0], mock_nsc_last_call()->statusReport);
	TEST_ASSERT_EQUAL_UINT(REQ_STATUS_CHECK_OPT_REPORT, dieStateTablePtr->dieState[TEST_CH][0].reqStatusCheckOpt);
}

static void test_check_req_status_none_uses_ready_busy(void)
{
	submit_phy_req(REQ_CODE_RESET, 3, 0, 0);
	dieStateTablePtr->dieState[TEST_CH][3].reqStatusCheckOpt = REQ_STATUS_CHECK_OPT_NONE;

	mock_nsc_set_ready_busy(MOCK_NSC_ALL_WAYS_READY & ~(1U << 3));
	TEST_ASSERT_EQUAL_UINT(REQ_STATUS_RUNNING, CheckReqStatus(TEST_CH, 3));

	mock_nsc_set_ready_busy(1U << 3);
	TEST_ASSERT_EQUAL_UINT(REQ_STATUS_DONE, CheckReqStatus(TEST_CH, 3));
}

static void test_check_req_status_rejects_unknown_option(void)
{
	submit_phy_req(REQ_CODE_WRITE, 0, TEST_BLOCK, TEST_PAGE);
	dieStateTablePtr->dieState[TEST_CH][0].reqStatusCheckOpt = 0xF;

	FW_EXPECT_ASSERT(CheckReqStatus(TEST_CH, 0));
}

/* ------------------------------------------------------------------------ */
/* ExecuteNandReq / IssueNandReq direct                                      */
/* ------------------------------------------------------------------------ */

static void test_execute_nand_req_running_in_exe_state_is_noop(void)
{
	submit_phy_req(REQ_CODE_WRITE, 0, TEST_BLOCK, TEST_PAGE);

	ExecuteNandReq(TEST_CH, 0, REQ_STATUS_RUNNING);
	TEST_ASSERT_EQUAL_UINT(DIE_STATE_EXE, dieStateTablePtr->dieState[TEST_CH][0].dieState);
	TEST_ASSERT_EQUAL_UINT(1, mock_nsc_count_cmd(V2FCommand_ProgramPage));

	ExecuteNandReq(TEST_CH, 0, REQ_STATUS_RUNNING);
	TEST_ASSERT_EQUAL_UINT(DIE_STATE_EXE, dieStateTablePtr->dieState[TEST_CH][0].dieState);
	TEST_ASSERT_EQUAL_UINT(1, mock_nsc_count_cmd(V2FCommand_ProgramPage));
	TEST_ASSERT_EQUAL_UINT(1, notCompletedNandReqCnt);
}

static void test_execute_nand_req_rejects_unknown_status(void)
{
	submit_phy_req(REQ_CODE_WRITE, 0, TEST_BLOCK, TEST_PAGE);
	ExecuteNandReq(TEST_CH, 0, REQ_STATUS_RUNNING);

	FW_EXPECT_ASSERT(ExecuteNandReq(TEST_CH, 0, 0x7));
}

static void test_execute_nand_req_warning_on_write_completes_request(void)
{
	submit_phy_req(REQ_CODE_WRITE, 1, TEST_BLOCK, TEST_PAGE);
	ExecuteNandReq(TEST_CH, 1, REQ_STATUS_RUNNING);
	retryLimitTablePtr->retryLimit[TEST_CH][1] = 2;

	ExecuteNandReq(TEST_CH, 1, REQ_STATUS_WARNING);

	TEST_ASSERT_EQUAL_UINT(0, notCompletedNandReqCnt);
	TEST_ASSERT_EQUAL_UINT(DIE_STATE_IDLE, dieStateTablePtr->dieState[TEST_CH][1].dieState);
	TEST_ASSERT_EQUAL_UINT(RETRY_LIMIT, retryLimitTablePtr->retryLimit[TEST_CH][1]);
	TEST_ASSERT_TRUE(phy_block_is_bad(1, TEST_BLOCK));
}

static void test_execute_nand_req_read_fail_with_exhausted_retries_fails_immediately(void)
{
	submit_phy_req(REQ_CODE_READ, 0, TEST_BLOCK, TEST_PAGE);
	ExecuteNandReq(TEST_CH, 0, REQ_STATUS_RUNNING);
	retryLimitTablePtr->retryLimit[TEST_CH][0] = 0;

	ExecuteNandReq(TEST_CH, 0, REQ_STATUS_FAIL);

	TEST_ASSERT_EQUAL_UINT(0, notCompletedNandReqCnt);
	TEST_ASSERT_TRUE(phy_block_is_bad(0, TEST_BLOCK));
	TEST_ASSERT_EQUAL_UINT(RETRY_LIMIT, retryLimitTablePtr->retryLimit[TEST_CH][0]);
}

static void test_issue_nand_req_rejects_unknown_req_code(void)
{
	unsigned int reqSlotTag = alloc_phy_req(REQ_CODE_FLUSH, 0, TEST_BLOCK, TEST_PAGE);

	PutToNandReqQ(reqSlotTag, TEST_CH, 0);
	FW_EXPECT_ASSERT(IssueNandReq(TEST_CH, 0));
}

static void test_put_to_way_priority_table_rejects_unknown_req_code(void)
{
	unsigned int reqSlotTag = alloc_phy_req(REQ_CODE_FLUSH, 0, TEST_BLOCK, TEST_PAGE);

	FW_EXPECT_ASSERT(PutToNandWayPriorityTable(reqSlotTag, TEST_CH, 0));
}

static void test_issue_nand_req_passes_generated_addresses_to_controller(void)
{
	unsigned int reqSlotTag = alloc_phy_req(REQ_CODE_WRITE, 2, TEST_BLOCK, TEST_PAGE);
	const mock_nsc_call_t *call;

	req(reqSlotTag)->reqOpt.dataBufFormat = REQ_OPT_DATA_BUF_ENTRY;
	req(reqSlotTag)->dataBufInfo.entry = 3;
	PutToNandReqQ(reqSlotTag, TEST_CH, 2);

	IssueNandReq(TEST_CH, 2);

	call = mock_nsc_last_call();
	TEST_ASSERT_EQUAL_UINT(V2FCommand_ProgramPage, call->cmd);
	TEST_ASSERT_EQUAL_UINT(TEST_CH, call->channel);
	TEST_ASSERT_EQUAL_INT(2, call->way);
	TEST_ASSERT_EQUAL_HEX32(GenerateNandRowAddr(reqSlotTag), call->rowAddress);
	TEST_ASSERT_EQUAL_PTR(fw_ptr(GenerateDataBufAddr(reqSlotTag)), call->pageDataBuffer);
	TEST_ASSERT_EQUAL_PTR(fw_ptr(GenerateSpareDataBufAddr(reqSlotTag)), call->spareDataBuffer);
	TEST_ASSERT_EQUAL_UINT(REQ_STATUS_CHECK_OPT_CHECK, dieStateTablePtr->dieState[TEST_CH][2].reqStatusCheckOpt);
}

static void test_issue_read_transfer_points_controller_at_error_and_completion_tables(void)
{
	unsigned int reqSlotTag = alloc_phy_req(REQ_CODE_READ_TRANSFER, 4, TEST_BLOCK, TEST_PAGE);
	const mock_nsc_call_t *call;

	PutToNandReqQ(reqSlotTag, TEST_CH, 4);
	IssueNandReq(TEST_CH, 4);

	call = mock_nsc_last_call();
	TEST_ASSERT_EQUAL_UINT(V2FCommand_ReadPageTransfer, call->cmd);
	TEST_ASSERT_EQUAL_PTR(&eccErrorInfoTablePtr->errorInfo[TEST_CH][4][0], call->errorInformation);
	TEST_ASSERT_EQUAL_PTR(&completeFlagTablePtr->completeFlag[TEST_CH][4], call->completion);
	TEST_ASSERT_EQUAL_UINT(1, completeFlagTablePtr->completeFlag[TEST_CH][4]);
}

/* ------------------------------------------------------------------------ */
/* Sync helpers                                                              */
/* ------------------------------------------------------------------------ */

static void test_sync_avail_free_req_waits_until_a_slot_is_released(void)
{
	unsigned int reqSlotTag = alloc_phy_req(REQ_CODE_ERASE, 0, TEST_BLOCK, 0);

	SelectLowLevelReqQ(reqSlotTag);
	while (freeReqQ.headReq != REQ_SLOT_TAG_NONE)
		GetFromFreeReqQ();

	SyncAvailFreeReq();

	TEST_ASSERT_EQUAL_UINT(reqSlotTag, freeReqQ.headReq);
	TEST_ASSERT_EQUAL_UINT(0, notCompletedNandReqCnt);
	TEST_ASSERT_EQUAL_UINT(1, mock_nsc_count_cmd(V2FCommand_BlockErase));
}

static void test_sync_avail_free_req_returns_immediately_when_slots_exist(void)
{
	SyncAvailFreeReq();
	TEST_ASSERT_EQUAL_UINT(0, mock_nsc_call_count());
}

static void test_sync_release_erase_req_runs_scheduler_until_erase_unblocked(void)
{
	unsigned int dieNo = 0;
	unsigned int blockNo = 3;
	unsigned int chNo = Vdie2PchTranslation(dieNo);
	unsigned int wayNo = Vdie2PwayTranslation(dieNo);
	unsigned int eraseReq = alloc_vsa_req(REQ_CODE_ERASE, dieNo, blockNo, 0);
	unsigned int writeReq;

	/* erase expects one programmed page, but none is permitted yet: blocked */
	req(eraseReq)->nandInfo.programmedPageCnt = 1;
	SelectLowLevelReqQ(eraseReq);
	TEST_ASSERT_EQUAL_UINT(1, rowAddrDependencyTablePtr->block[chNo][wayNo][blockNo].blockedEraseReqFlag);
	TEST_ASSERT_EQUAL_UINT(eraseReq, blockedByRowAddrDepReqQ[chNo][wayNo].headReq);
	TEST_ASSERT_EQUAL_UINT(1, blockedReqCnt);

	/* the write of page 0 makes the erase eligible once it drains */
	writeReq = alloc_vsa_req(REQ_CODE_WRITE, dieNo, blockNo, 0);
	SelectLowLevelReqQ(writeReq);
	TEST_ASSERT_EQUAL_UINT(writeReq, nandReqQ[chNo][wayNo].headReq);

	SyncReleaseEraseReq(chNo, wayNo, blockNo);

	TEST_ASSERT_EQUAL_UINT(0, rowAddrDependencyTablePtr->block[chNo][wayNo][blockNo].blockedEraseReqFlag);
	TEST_ASSERT_EQUAL_UINT(0, blockedReqCnt);
	TEST_ASSERT_EQUAL_UINT(1, mock_nsc_count_cmd(V2FCommand_ProgramPage));

	SyncAllLowLevelReqDone();
	TEST_ASSERT_EQUAL_UINT(1, mock_nsc_count_cmd(V2FCommand_BlockErase));
	TEST_ASSERT_EQUAL_UINT(0, notCompletedNandReqCnt);
}

static void test_sync_release_erase_req_returns_immediately_when_not_blocked(void)
{
	SyncReleaseEraseReq(TEST_CH, 0, TEST_BLOCK);
	TEST_ASSERT_EQUAL_UINT(0, mock_nsc_call_count());
}

int main(void)
{
	UNITY_BEGIN();
	RUN_TEST(test_smoke_scheduling_with_no_requests_issues_nothing);
	RUN_TEST(test_smoke_erase_request_is_issued_and_completed);

	RUN_TEST(test_write_walks_idle_write_statuscheck_statusreport_and_completes);
	RUN_TEST(test_read_becomes_read_transfer_and_completes);
	RUN_TEST(test_read_with_ecc_off_uses_raw_transfer);
	RUN_TEST(test_reset_and_set_feature_go_through_write_list_without_status_check);
	RUN_TEST(test_requests_on_all_ways_complete_and_idle_list_is_rebuilt);
	RUN_TEST(test_queued_requests_on_same_way_run_in_order);

	RUN_TEST(test_way_not_ready_keeps_request_in_status_report_list);
	RUN_TEST(test_way_not_ready_keeps_request_in_status_check_list);
	RUN_TEST(test_status_report_not_done_keeps_way_in_status_report_list);
	RUN_TEST(test_status_report_done_but_not_complete_reissues_status_check);
	RUN_TEST(test_controller_busy_after_write_stops_issuing_further_ways);
	RUN_TEST(test_controller_busy_after_erase_stops_issuing_further_ways);
	RUN_TEST(test_controller_busy_after_read_trigger_stops_issuing_further_ways);
	RUN_TEST(test_controller_busy_after_read_transfer_stops_issuing_further_ways);
	RUN_TEST(test_controller_busy_after_status_check_stops_checking_further_ways);
	RUN_TEST(test_controller_busy_with_idle_ways_only_skips_issue_block);

	RUN_TEST(test_write_status_fail_marks_grown_bad_block_and_completes);
	RUN_TEST(test_erase_status_fail_in_lun1_marks_correct_block);
	RUN_TEST(test_read_trigger_fail_retries_up_to_retry_limit);
	RUN_TEST(test_read_trigger_fail_with_ecc_off_and_buf_addr_writes_bad_mark);
	RUN_TEST(test_read_transfer_ecc_fail_retries_from_trigger_then_marks_bad);
	RUN_TEST(test_read_transfer_page_chunk_invalid_is_ecc_fail);
	RUN_TEST(test_read_transfer_ecc_warning_completes_and_marks_block_bad);
	RUN_TEST(test_ecc_warning_ignored_when_warning_option_off);
	RUN_TEST(test_check_ecc_error_info_classifies_error_words);

	RUN_TEST(test_check_req_status_completion_flag_paths);
	RUN_TEST(test_check_req_status_report_paths);
	RUN_TEST(test_check_req_status_none_uses_ready_busy);
	RUN_TEST(test_check_req_status_rejects_unknown_option);

	RUN_TEST(test_execute_nand_req_running_in_exe_state_is_noop);
	RUN_TEST(test_execute_nand_req_rejects_unknown_status);
	RUN_TEST(test_execute_nand_req_warning_on_write_completes_request);
	RUN_TEST(test_execute_nand_req_read_fail_with_exhausted_retries_fails_immediately);
	RUN_TEST(test_issue_nand_req_rejects_unknown_req_code);
	RUN_TEST(test_put_to_way_priority_table_rejects_unknown_req_code);
	RUN_TEST(test_issue_nand_req_passes_generated_addresses_to_controller);
	RUN_TEST(test_issue_read_transfer_points_controller_at_error_and_completion_tables);

	RUN_TEST(test_sync_avail_free_req_waits_until_a_slot_is_released);
	RUN_TEST(test_sync_avail_free_req_returns_immediately_when_slots_exist);
	RUN_TEST(test_sync_release_erase_req_runs_scheduler_until_erase_unblocked);
	RUN_TEST(test_sync_release_erase_req_returns_immediately_when_not_blocked);
	return UNITY_END();
}
