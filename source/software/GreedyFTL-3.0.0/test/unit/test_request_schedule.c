/* Unit tests for request_schedule.c. */
#include "unity.h"

#include "fw_test.h"
#include "memory_map.h"

void setUp(void)
{
	fw_test_reset();
	fw_test_init_ftl();
	mock_nsc_reset();
}

void tearDown(void) {}

static void test_smoke_scheduling_with_no_requests_issues_nothing(void)
{
	SchedulingNandReq();

	TEST_ASSERT_EQUAL_UINT(0, notCompletedNandReqCnt);
	TEST_ASSERT_EQUAL_UINT(0, mock_nsc_call_count());
}

static void test_smoke_erase_request_is_issued_and_completed(void)
{
	unsigned int reqSlotTag = GetFromFreeReqQ();

	reqPoolPtr->reqPool[reqSlotTag].reqType = REQ_TYPE_NAND;
	reqPoolPtr->reqPool[reqSlotTag].reqCode = REQ_CODE_ERASE;
	reqPoolPtr->reqPool[reqSlotTag].reqOpt.nandAddr = REQ_OPT_NAND_ADDR_PHY_ORG;
	reqPoolPtr->reqPool[reqSlotTag].reqOpt.dataBufFormat = REQ_OPT_DATA_BUF_NONE;
	reqPoolPtr->reqPool[reqSlotTag].reqOpt.rowAddrDependencyCheck = REQ_OPT_ROW_ADDR_DEPENDENCY_NONE;
	reqPoolPtr->reqPool[reqSlotTag].reqOpt.blockSpace = REQ_OPT_BLOCK_SPACE_TOTAL;
	reqPoolPtr->reqPool[reqSlotTag].nandInfo.physicalCh = 0;
	reqPoolPtr->reqPool[reqSlotTag].nandInfo.physicalWay = 0;
	reqPoolPtr->reqPool[reqSlotTag].nandInfo.physicalBlock = 1;
	reqPoolPtr->reqPool[reqSlotTag].nandInfo.physicalPage = 0;
	reqPoolPtr->reqPool[reqSlotTag].prevBlockingReq = REQ_SLOT_TAG_NONE;

	SelectLowLevelReqQ(reqSlotTag);
	SyncAllLowLevelReqDone();

	TEST_ASSERT_EQUAL_UINT(1, mock_nsc_count_cmd(V2FCommand_BlockErase));
	TEST_ASSERT_EQUAL_UINT(0, notCompletedNandReqCnt);
}

int main(void)
{
	UNITY_BEGIN();
	RUN_TEST(test_smoke_scheduling_with_no_requests_issues_nothing);
	RUN_TEST(test_smoke_erase_request_is_issued_and_completed);
	return UNITY_END();
}
