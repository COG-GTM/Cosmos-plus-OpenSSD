#include "test_support.h"

void setUp(void)
{
	test_ftl_init();
}

void tearDown(void)
{
}

static unsigned int nandReq(unsigned int reqCode, unsigned int vsa)
{
	unsigned int tag = GetFromFreeReqQ();

	reqPoolPtr->reqPool[tag].reqType = REQ_TYPE_NAND;
	reqPoolPtr->reqPool[tag].reqCode = reqCode;
	reqPoolPtr->reqPool[tag].nvmeCmdSlotTag = 0;
	reqPoolPtr->reqPool[tag].logicalSliceAddr = LSA_NONE;
	reqPoolPtr->reqPool[tag].reqOpt.dataBufFormat = REQ_OPT_DATA_BUF_ENTRY;
	reqPoolPtr->reqPool[tag].reqOpt.nandAddr = REQ_OPT_NAND_ADDR_VSA;
	reqPoolPtr->reqPool[tag].reqOpt.nandEcc = REQ_OPT_NAND_ECC_ON;
	reqPoolPtr->reqPool[tag].reqOpt.nandEccWarning = REQ_OPT_NAND_ECC_WARNING_ON;
	reqPoolPtr->reqPool[tag].reqOpt.rowAddrDependencyCheck = REQ_OPT_ROW_ADDR_DEPENDENCY_CHECK;
	reqPoolPtr->reqPool[tag].reqOpt.blockSpace = REQ_OPT_BLOCK_SPACE_MAIN;
	reqPoolPtr->reqPool[tag].dataBufInfo.entry = 0;
	reqPoolPtr->reqPool[tag].nandInfo.virtualSliceAddr = vsa;
	reqPoolPtr->reqPool[tag].nandInfo.programmedPageCnt = 0;
	return tag;
}

/* ---- scheduler bookkeeping --------------------------------------------------- */

static void test_init_puts_every_way_on_idle_list(void)
{
	unsigned int chNo, wayNo;

	for (chNo = 0; chNo < USER_CHANNELS; chNo++)
	{
		TEST_ASSERT_EQUAL_UINT(0, wayPriorityTablePtr->wayPriority[chNo].idleHead);
		TEST_ASSERT_EQUAL_UINT(USER_WAYS - 1, wayPriorityTablePtr->wayPriority[chNo].idleTail);
		TEST_ASSERT_EQUAL_UINT(WAY_NONE, wayPriorityTablePtr->wayPriority[chNo].writeHead);
		TEST_ASSERT_EQUAL_UINT(WAY_NONE, wayPriorityTablePtr->wayPriority[chNo].eraseHead);
		for (wayNo = 0; wayNo < USER_WAYS; wayNo++)
			TEST_ASSERT_EQUAL_UINT(DIE_STATE_IDLE, dieStateTablePtr->dieState[chNo][wayNo].dieState);
	}
}

static void test_way_priority_lists_are_doubly_linked(void)
{
	SelectivGetFromNandIdleList(0, 3);
	PutToNandWriteList(0, 3);
	TEST_ASSERT_EQUAL_UINT(3, wayPriorityTablePtr->wayPriority[0].writeHead);
	TEST_ASSERT_EQUAL_UINT(3, wayPriorityTablePtr->wayPriority[0].writeTail);
	TEST_ASSERT_EQUAL_UINT(4, dieStateTablePtr->dieState[0][2].nextWay);
	TEST_ASSERT_EQUAL_UINT(2, dieStateTablePtr->dieState[0][4].prevWay);

	SelectivGetFromNandIdleList(0, 0);
	PutToNandWriteList(0, 0);
	TEST_ASSERT_EQUAL_UINT(1, wayPriorityTablePtr->wayPriority[0].idleHead);
	TEST_ASSERT_EQUAL_UINT(3, wayPriorityTablePtr->wayPriority[0].writeHead);
	TEST_ASSERT_EQUAL_UINT(0, wayPriorityTablePtr->wayPriority[0].writeTail);
	TEST_ASSERT_EQUAL_UINT(0, dieStateTablePtr->dieState[0][3].nextWay);

	SelectiveGetFromNandWriteList(0, 3);
	TEST_ASSERT_EQUAL_UINT(0, wayPriorityTablePtr->wayPriority[0].writeHead);
	PutToNandIdleList(0, 3);
	TEST_ASSERT_EQUAL_UINT(3, wayPriorityTablePtr->wayPriority[0].idleTail);
}

/* ---- address generation ------------------------------------------------------ */

static void test_row_addr_for_vsa_matches_physical_remap(void)
{
	unsigned int vsa = Vorg2VsaTranslation(5, 17, 9);
	unsigned int tag = nandReq(REQ_CODE_READ, vsa);
	PSA psa = test_vsa_to_psa(vsa);

	TEST_ASSERT_EQUAL_HEX32(psa.rowAddr, GenerateNandRowAddr(tag));
	TEST_ASSERT_EQUAL_UINT(Vpage2PlsbPageTranslation(9), GenerateNandRowAddr(tag) % PAGES_PER_MLC_BLOCK);
}

static void test_row_addr_in_second_lun_uses_lun1_base(void)
{
	unsigned int tag = nandReq(REQ_CODE_READ, 0);

	reqPoolPtr->reqPool[tag].reqOpt.nandAddr = REQ_OPT_NAND_ADDR_PHY_ORG;
	reqPoolPtr->reqPool[tag].reqOpt.blockSpace = REQ_OPT_BLOCK_SPACE_TOTAL;
	reqPoolPtr->reqPool[tag].nandInfo.physicalCh = 1;
	reqPoolPtr->reqPool[tag].nandInfo.physicalWay = 2;
	reqPoolPtr->reqPool[tag].nandInfo.physicalBlock = TOTAL_BLOCKS_PER_LUN + 3;
	reqPoolPtr->reqPool[tag].nandInfo.physicalPage = 4;

	TEST_ASSERT_EQUAL_HEX32(LUN_1_BASE_ADDR + 3 * PAGES_PER_MLC_BLOCK + 4, GenerateNandRowAddr(tag));

	reqPoolPtr->reqPool[tag].nandInfo.physicalBlock = 3;
	TEST_ASSERT_EQUAL_HEX32(LUN_0_BASE_ADDR + 3 * PAGES_PER_MLC_BLOCK + 4, GenerateNandRowAddr(tag));
}

static void test_data_buf_addr_selects_region_by_format(void)
{
	unsigned int tag = nandReq(REQ_CODE_READ, 0);

	reqPoolPtr->reqPool[tag].dataBufInfo.entry = 3;
	TEST_ASSERT_EQUAL_HEX32(DATA_BUFFER_BASE_ADDR + 3 * BYTES_PER_DATA_REGION_OF_SLICE, GenerateDataBufAddr(tag));
	TEST_ASSERT_EQUAL_HEX32(SPARE_DATA_BUFFER_BASE_ADDR + 3 * BYTES_PER_SPARE_REGION_OF_SLICE, GenerateSpareDataBufAddr(tag));

	reqPoolPtr->reqPool[tag].reqOpt.dataBufFormat = REQ_OPT_DATA_BUF_TEMP_ENTRY;
	TEST_ASSERT_EQUAL_HEX32(TEMPORARY_DATA_BUFFER_BASE_ADDR + 3 * BYTES_PER_DATA_REGION_OF_SLICE, GenerateDataBufAddr(tag));

	reqPoolPtr->reqPool[tag].reqOpt.dataBufFormat = REQ_OPT_DATA_BUF_ADDR;
	reqPoolPtr->reqPool[tag].dataBufInfo.addr = 0x1234000;
	TEST_ASSERT_EQUAL_HEX32(0x1234000, GenerateDataBufAddr(tag));
	TEST_ASSERT_EQUAL_HEX32(0x1234000 + BYTES_PER_DATA_REGION_OF_SLICE, GenerateSpareDataBufAddr(tag));

	reqPoolPtr->reqPool[tag].reqOpt.dataBufFormat = REQ_OPT_DATA_BUF_NONE;
	TEST_ASSERT_EQUAL_HEX32(RESERVED_DATA_BUFFER_BASE_ADDR, GenerateDataBufAddr(tag));

	reqPoolPtr->reqPool[tag].reqType = REQ_TYPE_NVME_DMA;
	reqPoolPtr->reqPool[tag].reqOpt.dataBufFormat = REQ_OPT_DATA_BUF_ENTRY;
	reqPoolPtr->reqPool[tag].dataBufInfo.entry = 3;	/* dataBufInfo is a union; addr overwrote it */
	reqPoolPtr->reqPool[tag].nvmeDmaInfo.nvmeBlockOffset = 2;
	TEST_ASSERT_EQUAL_HEX32(DATA_BUFFER_BASE_ADDR + 3 * BYTES_PER_DATA_REGION_OF_SLICE + 2 * BYTES_PER_NVME_BLOCK,
							GenerateDataBufAddr(tag));
}

/* ---- row-address dependency -------------------------------------------------- */

static void test_writes_must_follow_page_order_within_a_block(void)
{
	unsigned int blockNo = 40, w0, w2, w1;

	w0 = nandReq(REQ_CODE_WRITE, Vorg2VsaTranslation(0, blockNo, 0));
	w2 = nandReq(REQ_CODE_WRITE, Vorg2VsaTranslation(0, blockNo, 2));
	w1 = nandReq(REQ_CODE_WRITE, Vorg2VsaTranslation(0, blockNo, 1));

	TEST_ASSERT_EQUAL_UINT(ROW_ADDR_DEPENDENCY_REPORT_PASS, CheckRowAddrDep(w0, ROW_ADDR_DEPENDENCY_CHECK_OPT_SELECT));
	TEST_ASSERT_EQUAL_UINT(ROW_ADDR_DEPENDENCY_REPORT_BLOCKED, CheckRowAddrDep(w2, ROW_ADDR_DEPENDENCY_CHECK_OPT_SELECT));
	TEST_ASSERT_EQUAL_UINT(ROW_ADDR_DEPENDENCY_REPORT_PASS, CheckRowAddrDep(w1, ROW_ADDR_DEPENDENCY_CHECK_OPT_SELECT));
	TEST_ASSERT_EQUAL_UINT(ROW_ADDR_DEPENDENCY_REPORT_PASS, CheckRowAddrDep(w2, ROW_ADDR_DEPENDENCY_CHECK_OPT_SELECT));
	TEST_ASSERT_EQUAL_UINT(3, rowAddrDependencyTablePtr->block[0][0][blockNo].permittedProgPage);
}

static void test_read_of_unprogrammed_page_is_blocked_until_written(void)
{
	unsigned int blockNo = 41, read, write;

	read = nandReq(REQ_CODE_READ, Vorg2VsaTranslation(0, blockNo, 0));
	write = nandReq(REQ_CODE_WRITE, Vorg2VsaTranslation(0, blockNo, 0));

	TEST_ASSERT_EQUAL_UINT(ROW_ADDR_DEPENDENCY_REPORT_BLOCKED, CheckRowAddrDep(read, ROW_ADDR_DEPENDENCY_CHECK_OPT_SELECT));
	TEST_ASSERT_EQUAL_UINT(1, rowAddrDependencyTablePtr->block[0][0][blockNo].blockedReadReqCnt);

	TEST_ASSERT_EQUAL_UINT(ROW_ADDR_DEPENDENCY_REPORT_PASS, CheckRowAddrDep(write, ROW_ADDR_DEPENDENCY_CHECK_OPT_SELECT));
	TEST_ASSERT_EQUAL_UINT(ROW_ADDR_DEPENDENCY_REPORT_PASS, CheckRowAddrDep(read, ROW_ADDR_DEPENDENCY_CHECK_OPT_RELEASE));
	TEST_ASSERT_EQUAL_UINT(0, rowAddrDependencyTablePtr->block[0][0][blockNo].blockedReadReqCnt);
}

static void test_erase_waits_for_outstanding_reads_then_resets_block(void)
{
	unsigned int blockNo = 42, write, read, erase;

	write = nandReq(REQ_CODE_WRITE, Vorg2VsaTranslation(0, blockNo, 0));
	read = nandReq(REQ_CODE_READ, Vorg2VsaTranslation(0, blockNo, 1));
	erase = nandReq(REQ_CODE_ERASE, Vorg2VsaTranslation(0, blockNo, 0));
	reqPoolPtr->reqPool[erase].nandInfo.programmedPageCnt = 1;

	CheckRowAddrDep(write, ROW_ADDR_DEPENDENCY_CHECK_OPT_SELECT);
	CheckRowAddrDep(read, ROW_ADDR_DEPENDENCY_CHECK_OPT_SELECT);

	TEST_ASSERT_EQUAL_UINT(ROW_ADDR_DEPENDENCY_REPORT_BLOCKED, CheckRowAddrDep(erase, ROW_ADDR_DEPENDENCY_CHECK_OPT_SELECT));
	TEST_ASSERT_EQUAL_UINT(1, rowAddrDependencyTablePtr->block[0][0][blockNo].blockedEraseReqFlag);

	rowAddrDependencyTablePtr->block[0][0][blockNo].blockedReadReqCnt = 0;
	TEST_ASSERT_EQUAL_UINT(ROW_ADDR_DEPENDENCY_REPORT_PASS, CheckRowAddrDep(erase, ROW_ADDR_DEPENDENCY_CHECK_OPT_RELEASE));
	TEST_ASSERT_EQUAL_UINT(0, rowAddrDependencyTablePtr->block[0][0][blockNo].permittedProgPage);
	TEST_ASSERT_EQUAL_UINT(0, rowAddrDependencyTablePtr->block[0][0][blockNo].blockedEraseReqFlag);
}

static void test_erase_with_wrong_programmed_count_stays_blocked(void)
{
	unsigned int blockNo = 43, erase;

	erase = nandReq(REQ_CODE_ERASE, Vorg2VsaTranslation(0, blockNo, 0));
	reqPoolPtr->reqPool[erase].nandInfo.programmedPageCnt = 5;

	TEST_ASSERT_EQUAL_UINT(ROW_ADDR_DEPENDENCY_REPORT_BLOCKED, CheckRowAddrDep(erase, ROW_ADDR_DEPENDENCY_CHECK_OPT_SELECT));
	TEST_ASSERT_EQUAL_UINT(1, rowAddrDependencyTablePtr->block[0][0][blockNo].blockedEraseReqFlag);
}

/* ---- end-to-end issue through the scheduler ---------------------------------- */

static void test_select_low_level_q_issues_ready_write_and_blocks_out_of_order_one(void)
{
	unsigned int blockNo = 44, w1;

	fake_nand_reset_stats();
	w1 = nandReq(REQ_CODE_WRITE, Vorg2VsaTranslation(0, blockNo, 1));
	SelectLowLevelReqQ(w1);

	TEST_ASSERT_EQUAL_UINT(1, blockedReqCnt);
	TEST_ASSERT_EQUAL_UINT(0, notCompletedNandReqCnt);

	test_program_vsa(700, Vorg2VsaTranslation(0, blockNo, 0), 0x11);

	TEST_ASSERT_EQUAL_UINT(0, blockedReqCnt);
	TEST_ASSERT_EQUAL_UINT(2, fake_nand_stats()->programs);
	TEST_ASSERT_EQUAL_UINT(AVAILABLE_OUNTSTANDING_REQ_COUNT, freeReqQ.reqCnt);
}

static void test_reads_on_different_channels_issue_in_parallel(void)
{
	unsigned int vsaA = test_write_slice(800, 0xAA);
	unsigned int vsaB = test_write_slice(801, 0xBB);
	unsigned int rA, rB;

	TEST_ASSERT_NOT_EQUAL_UINT(Vdie2PchTranslation(Vsa2VdieTranslation(vsaA)),
							   Vdie2PchTranslation(Vsa2VdieTranslation(vsaB)));

	fake_nand_reset_stats();
	rA = nandReq(REQ_CODE_READ, vsaA);
	rB = nandReq(REQ_CODE_READ, vsaB);
	reqPoolPtr->reqPool[rA].dataBufInfo.entry = AllocateDataBuf();
	reqPoolPtr->reqPool[rB].dataBufInfo.entry = AllocateDataBuf();
	SelectLowLevelReqQ(rA);
	SelectLowLevelReqQ(rB);
	TEST_ASSERT_EQUAL_UINT(2, notCompletedNandReqCnt);

	SchedulingNandReq();
	TEST_ASSERT_EQUAL_UINT(2, fake_nand_stats()->readTriggers);

	test_drain_nand();
	TEST_ASSERT_EQUAL_UINT(2, fake_nand_stats()->readTransfers);
	TEST_ASSERT_EQUAL_UINT(0, notCompletedNandReqCnt);
	TEST_ASSERT_EQUAL_HEX8(0xAA, *(unsigned char *)host_mem_ptr(GenerateDataBufAddr(rA)));
	TEST_ASSERT_EQUAL_HEX8(0xBB, *(unsigned char *)host_mem_ptr(GenerateDataBufAddr(rB)));
}

int main(void)
{
	UNITY_BEGIN();
	RUN_TEST(test_init_puts_every_way_on_idle_list);
	RUN_TEST(test_way_priority_lists_are_doubly_linked);
	RUN_TEST(test_row_addr_for_vsa_matches_physical_remap);
	RUN_TEST(test_row_addr_in_second_lun_uses_lun1_base);
	RUN_TEST(test_data_buf_addr_selects_region_by_format);
	RUN_TEST(test_writes_must_follow_page_order_within_a_block);
	RUN_TEST(test_read_of_unprogrammed_page_is_blocked_until_written);
	RUN_TEST(test_erase_waits_for_outstanding_reads_then_resets_block);
	RUN_TEST(test_erase_with_wrong_programmed_count_stays_blocked);
	RUN_TEST(test_select_low_level_q_issues_ready_write_and_blocks_out_of_order_one);
	RUN_TEST(test_reads_on_different_channels_issue_in_parallel);
	return UNITY_END();
}
