#include <string.h>

#include "unity.h"

#include "ftl_test_env.h"

/*
 * request_schedule.c / request_transform.c: NAND request completion handling
 * (ECC warning, uncorrectable read, erase failure), address generation for the
 * different request formats, and row-address dependency blocking.
 */

void setUp(void)
{
	ftl_test_env_init();
}

void tearDown(void)
{
}

static unsigned int NewNandReq(unsigned int reqCode, unsigned int vsa, unsigned int eccWarning)
{
	unsigned int die = Vsa2VdieTranslation(vsa);
	unsigned int tag = GetFromFreeReqQ();

	reqPoolPtr->reqPool[tag].reqType = REQ_TYPE_NAND;
	reqPoolPtr->reqPool[tag].reqCode = reqCode;
	reqPoolPtr->reqPool[tag].logicalSliceAddr = LSA_NONE;
	reqPoolPtr->reqPool[tag].reqOpt.nandAddr = REQ_OPT_NAND_ADDR_VSA;
	reqPoolPtr->reqPool[tag].reqOpt.nandEcc = REQ_OPT_NAND_ECC_ON;
	reqPoolPtr->reqPool[tag].reqOpt.nandEccWarning = eccWarning;
	reqPoolPtr->reqPool[tag].reqOpt.rowAddrDependencyCheck = REQ_OPT_ROW_ADDR_DEPENDENCY_CHECK;
	reqPoolPtr->reqPool[tag].reqOpt.blockSpace = REQ_OPT_BLOCK_SPACE_MAIN;
	reqPoolPtr->reqPool[tag].nandInfo.virtualSliceAddr = vsa;

	if (reqCode == REQ_CODE_ERASE)
	{
		reqPoolPtr->reqPool[tag].reqOpt.dataBufFormat = REQ_OPT_DATA_BUF_NONE;
	}
	else
	{
		unsigned int entry = AllocateTempDataBuf(die);
		reqPoolPtr->reqPool[tag].reqOpt.dataBufFormat = REQ_OPT_DATA_BUF_TEMP_ENTRY;
		reqPoolPtr->reqPool[tag].dataBufInfo.entry = entry;
		UpdateTempDataBufEntryInfoBlockingReq(entry, tag);
	}
	return tag;
}

static unsigned int IssueRead(unsigned int vsa, unsigned int eccWarning)
{
	unsigned int tag = NewNandReq(REQ_CODE_READ, vsa, eccWarning);
	SelectLowLevelReqQ(tag);
	return tag;
}

/*
 * Program an explicit virtual slice, bypassing the logical-to-virtual map.
 * The payload comes from the reserved data buffer via an absolute address so
 * the write never joins the per-die temporary-buffer dependency chain.
 */
static unsigned int IssueRawWrite(unsigned int vsa, unsigned char fillByte)
{
	unsigned int tag = NewNandReq(REQ_CODE_ERASE, vsa, REQ_OPT_NAND_ECC_WARNING_OFF);
	reqPoolPtr->reqPool[tag].reqCode = REQ_CODE_WRITE;
	reqPoolPtr->reqPool[tag].reqOpt.dataBufFormat = REQ_OPT_DATA_BUF_ADDR;
	reqPoolPtr->reqPool[tag].dataBufInfo.addr = RESERVED_DATA_BUFFER_BASE_ADDR;
	memset(host_memory_ptr(GenerateDataBufAddr(tag)), fillByte, BYTES_PER_DATA_REGION_OF_SLICE);
	SelectLowLevelReqQ(tag);
	return tag;
}

static unsigned int PhyBlockOfVsa(unsigned int vsa)
{
	unsigned int die = Vsa2VdieTranslation(vsa);
	unsigned int vblock = Vsa2VblockTranslation(vsa);
	return phyBlockMapPtr->phyBlock[die][Vblock2PblockOfTbsTranslation(vblock)].remappedPhyBlock;
}

/* ---- ECC results on read ------------------------------------------------- */

static void test_read_with_many_corrected_bits_completes_but_books_grown_bad_block(void)
{
	unsigned int vsa = ftl_test_write_slice(0, 0xA5);
	unsigned int die = Vsa2VdieTranslation(vsa);
	unsigned int ch = Vdie2PchTranslation(die), way = Vdie2PwayTranslation(die);
	unsigned int tag;

	fake_nand_set_ecc_result(ch, way, BIT_ERROR_THRESHOLD_PER_CHUNK + 1, 0);
	tag = IssueRead(vsa, REQ_OPT_NAND_ECC_WARNING_ON);
	ftl_test_drain();

	TEST_ASSERT_EQUAL_UINT8(0xA5, ((unsigned char *)host_memory_ptr(GenerateDataBufAddr(tag)))[0]);
	TEST_ASSERT_EQUAL_UINT32(BLOCK_STATE_BAD, phyBlockMapPtr->phyBlock[die][PhyBlockOfVsa(vsa)].bad);
	TEST_ASSERT_EQUAL_UINT32(BBT_INFO_GROWN_BAD_UPDATE_BOOKED, bbtInfoMapPtr->bbtInfo[die].grownBadUpdate);
	TEST_ASSERT_EQUAL_UINT32(RETRY_LIMIT, retryLimitTablePtr->retryLimit[ch][way]);
}

static void test_ecc_warning_is_ignored_when_request_does_not_ask_for_it(void)
{
	unsigned int vsa = ftl_test_write_slice(0, 0x5A);
	unsigned int die = Vsa2VdieTranslation(vsa);

	fake_nand_set_ecc_result(Vdie2PchTranslation(die), Vdie2PwayTranslation(die), BIT_ERROR_THRESHOLD_PER_CHUNK + 1, 0);
	IssueRead(vsa, REQ_OPT_NAND_ECC_WARNING_OFF);
	ftl_test_drain();

	TEST_ASSERT_EQUAL_UINT32(BLOCK_STATE_NORMAL, phyBlockMapPtr->phyBlock[die][PhyBlockOfVsa(vsa)].bad);
	TEST_ASSERT_EQUAL_UINT32(BBT_INFO_GROWN_BAD_UPDATE_NONE, bbtInfoMapPtr->bbtInfo[die].grownBadUpdate);
}

static void test_corrected_bits_at_threshold_do_not_trigger_warning(void)
{
	unsigned int vsa = ftl_test_write_slice(0, 0x11);
	unsigned int die = Vsa2VdieTranslation(vsa);

	fake_nand_set_ecc_result(Vdie2PchTranslation(die), Vdie2PwayTranslation(die), BIT_ERROR_THRESHOLD_PER_CHUNK, 0);
	IssueRead(vsa, REQ_OPT_NAND_ECC_WARNING_ON);
	ftl_test_drain();

	TEST_ASSERT_EQUAL_UINT32(BLOCK_STATE_NORMAL, phyBlockMapPtr->phyBlock[die][PhyBlockOfVsa(vsa)].bad);
}

static void test_uncorrectable_read_is_retried_then_marks_block_bad(void)
{
	unsigned int vsa = ftl_test_write_slice(0, 0x33);
	unsigned int die = Vsa2VdieTranslation(vsa);
	unsigned int ch = Vdie2PchTranslation(die), way = Vdie2PwayTranslation(die);
	FAKE_NAND_DIE_STATS before = fake_nand_stats(ch, way), after;

	fake_nand_set_ecc_result(ch, way, 0, 1);
	IssueRead(vsa, REQ_OPT_NAND_ECC_WARNING_OFF);
	ftl_test_drain();
	after = fake_nand_stats(ch, way);

	/* First attempt plus RETRY_LIMIT retries, each a fresh trigger + transfer. */
	TEST_ASSERT_EQUAL_UINT32(RETRY_LIMIT + 1, after.readTriggers - before.readTriggers);
	TEST_ASSERT_EQUAL_UINT32(RETRY_LIMIT + 1, after.readTransfers - before.readTransfers);
	TEST_ASSERT_EQUAL_UINT32(BLOCK_STATE_BAD, phyBlockMapPtr->phyBlock[die][PhyBlockOfVsa(vsa)].bad);
	TEST_ASSERT_EQUAL_UINT32(BBT_INFO_GROWN_BAD_UPDATE_BOOKED, bbtInfoMapPtr->bbtInfo[die].grownBadUpdate);
	TEST_ASSERT_EQUAL_UINT32(RETRY_LIMIT, retryLimitTablePtr->retryLimit[ch][way]);
	TEST_ASSERT_EQUAL_UINT32(0, ftl_test_pending_nand_reqs());
}

static void test_retry_budget_is_per_die_and_restored_after_failure(void)
{
	unsigned int vsa = ftl_test_write_slice(0, 0x44);
	unsigned int die = Vsa2VdieTranslation(vsa);
	unsigned int ch = Vdie2PchTranslation(die), way = Vdie2PwayTranslation(die);
	unsigned int otherDie = (die + 1) % USER_DIES;

	fake_nand_set_ecc_result(ch, way, 0, 1);
	IssueRead(vsa, REQ_OPT_NAND_ECC_WARNING_OFF);
	ftl_test_drain();

	/* A second uncorrectable read on the same die gets the full budget again. */
	FAKE_NAND_DIE_STATS before = fake_nand_stats(ch, way);
	IssueRead(vsa, REQ_OPT_NAND_ECC_WARNING_OFF);
	ftl_test_drain();
	TEST_ASSERT_EQUAL_UINT32(RETRY_LIMIT + 1, fake_nand_stats(ch, way).readTriggers - before.readTriggers);

	TEST_ASSERT_EQUAL_UINT32(RETRY_LIMIT, retryLimitTablePtr->retryLimit[Vdie2PchTranslation(otherDie)][Vdie2PwayTranslation(otherDie)]);
	TEST_ASSERT_EQUAL_UINT32(BBT_INFO_GROWN_BAD_UPDATE_NONE, bbtInfoMapPtr->bbtInfo[otherDie].grownBadUpdate);
}

/* ---- Erase failure -------------------------------------------------------- */

static void test_erase_failure_marks_physical_block_bad(void)
{
	unsigned int vsa = ftl_test_write_slice(0, 0x77);
	unsigned int die = Vsa2VdieTranslation(vsa);
	unsigned int ch = Vdie2PchTranslation(die), way = Vdie2PwayTranslation(die);
	unsigned int vblock = Vsa2VblockTranslation(vsa);
	unsigned int tag;

	/* Erase the block the slice went to; the dependency check needs the programmed page count. */
	tag = NewNandReq(REQ_CODE_ERASE, ftl_test_vsa(die, vblock, 0), REQ_OPT_NAND_ECC_WARNING_OFF);
	reqPoolPtr->reqPool[tag].nandInfo.programmedPageCnt = rowAddrDependencyTablePtr->block[ch][way][vblock].permittedProgPage;
	fake_nand_fail_next_erase(ch, way);
	SelectLowLevelReqQ(tag);
	ftl_test_drain();

	TEST_ASSERT_EQUAL_UINT32(BLOCK_STATE_BAD, phyBlockMapPtr->phyBlock[die][PhyBlockOfVsa(vsa)].bad);
	TEST_ASSERT_EQUAL_UINT32(BBT_INFO_GROWN_BAD_UPDATE_BOOKED, bbtInfoMapPtr->bbtInfo[die].grownBadUpdate);
	TEST_ASSERT_EQUAL_UINT32(0, rowAddrDependencyTablePtr->block[ch][way][vblock].permittedProgPage);
	TEST_ASSERT_EQUAL_UINT32(0, ftl_test_pending_nand_reqs());
}

/* ---- Address generation --------------------------------------------------- */

static void test_row_addr_for_main_block_space_applies_bad_block_remap(void)
{
	unsigned int tag = GetFromFreeReqQ();
	unsigned int die = 2, ch = Vdie2PchTranslation(die), way = Vdie2PwayTranslation(die);
	unsigned int remapped = phyBlockMapPtr->phyBlock[die][0].remappedPhyBlock;

	/* Physical block 0 hosts the BBT and is always remapped into the reserved area. */
	TEST_ASSERT_EQUAL_UINT32(USER_BLOCKS_PER_LUN, remapped);

	reqPoolPtr->reqPool[tag].reqOpt.nandAddr = REQ_OPT_NAND_ADDR_PHY_ORG;
	reqPoolPtr->reqPool[tag].reqOpt.blockSpace = REQ_OPT_BLOCK_SPACE_MAIN;
	reqPoolPtr->reqPool[tag].nandInfo.physicalCh = ch;
	reqPoolPtr->reqPool[tag].nandInfo.physicalWay = way;
	reqPoolPtr->reqPool[tag].nandInfo.physicalBlock = 0;
	reqPoolPtr->reqPool[tag].nandInfo.physicalPage = 3;
	TEST_ASSERT_EQUAL_UINT32(LUN_0_BASE_ADDR + remapped * PAGES_PER_MLC_BLOCK + 3, GenerateNandRowAddr(tag));

	/* Main-space block numbers skip the reserved blocks: MAIN_BLOCKS_PER_LUN + 5 is LUN1 block 5. */
	reqPoolPtr->reqPool[tag].nandInfo.physicalBlock = MAIN_BLOCKS_PER_LUN + 5;
	reqPoolPtr->reqPool[tag].nandInfo.physicalPage = 9;
	TEST_ASSERT_EQUAL_UINT32(LUN_1_BASE_ADDR + 5 * PAGES_PER_MLC_BLOCK + 9, GenerateNandRowAddr(tag));

	PutToFreeReqQ(tag);
}

static void test_row_addr_for_total_block_space_is_not_remapped(void)
{
	unsigned int tag = GetFromFreeReqQ();

	reqPoolPtr->reqPool[tag].reqOpt.nandAddr = REQ_OPT_NAND_ADDR_PHY_ORG;
	reqPoolPtr->reqPool[tag].reqOpt.blockSpace = REQ_OPT_BLOCK_SPACE_TOTAL;
	reqPoolPtr->reqPool[tag].nandInfo.physicalCh = 0;
	reqPoolPtr->reqPool[tag].nandInfo.physicalWay = 0;
	reqPoolPtr->reqPool[tag].nandInfo.physicalBlock = 0;
	reqPoolPtr->reqPool[tag].nandInfo.physicalPage = 1;
	TEST_ASSERT_EQUAL_UINT32(LUN_0_BASE_ADDR + 1, GenerateNandRowAddr(tag));

	reqPoolPtr->reqPool[tag].nandInfo.physicalBlock = TOTAL_BLOCKS_PER_LUN + 7;
	reqPoolPtr->reqPool[tag].nandInfo.physicalPage = 0;
	TEST_ASSERT_EQUAL_UINT32(LUN_1_BASE_ADDR + 7 * PAGES_PER_MLC_BLOCK, GenerateNandRowAddr(tag));

	PutToFreeReqQ(tag);
}

static void test_data_buf_addr_follows_request_format(void)
{
	unsigned int tag = GetFromFreeReqQ();

	reqPoolPtr->reqPool[tag].reqType = REQ_TYPE_NAND;
	reqPoolPtr->reqPool[tag].dataBufInfo.entry = 3;

	reqPoolPtr->reqPool[tag].reqOpt.dataBufFormat = REQ_OPT_DATA_BUF_ENTRY;
	TEST_ASSERT_EQUAL_UINT32(DATA_BUFFER_BASE_ADDR + 3 * BYTES_PER_DATA_REGION_OF_SLICE, GenerateDataBufAddr(tag));
	TEST_ASSERT_EQUAL_UINT32(SPARE_DATA_BUFFER_BASE_ADDR + 3 * BYTES_PER_SPARE_REGION_OF_SLICE, GenerateSpareDataBufAddr(tag));

	reqPoolPtr->reqPool[tag].reqOpt.dataBufFormat = REQ_OPT_DATA_BUF_TEMP_ENTRY;
	TEST_ASSERT_EQUAL_UINT32(TEMPORARY_DATA_BUFFER_BASE_ADDR + 3 * BYTES_PER_DATA_REGION_OF_SLICE, GenerateDataBufAddr(tag));
	TEST_ASSERT_EQUAL_UINT32(TEMPORARY_SPARE_DATA_BUFFER_BASE_ADDR + 3 * BYTES_PER_SPARE_REGION_OF_SLICE, GenerateSpareDataBufAddr(tag));

	reqPoolPtr->reqPool[tag].reqOpt.dataBufFormat = REQ_OPT_DATA_BUF_ADDR;
	reqPoolPtr->reqPool[tag].dataBufInfo.addr = 0x1234000;
	TEST_ASSERT_EQUAL_UINT32(0x1234000, GenerateDataBufAddr(tag));
	TEST_ASSERT_EQUAL_UINT32(0x1234000 + BYTES_PER_DATA_REGION_OF_SLICE, GenerateSpareDataBufAddr(tag));

	reqPoolPtr->reqPool[tag].reqOpt.dataBufFormat = REQ_OPT_DATA_BUF_NONE;
	TEST_ASSERT_EQUAL_UINT32(RESERVED_DATA_BUFFER_BASE_ADDR, GenerateDataBufAddr(tag));
	TEST_ASSERT_EQUAL_UINT32(RESERVED_DATA_BUFFER_BASE_ADDR + BYTES_PER_DATA_REGION_OF_SLICE, GenerateSpareDataBufAddr(tag));

	reqPoolPtr->reqPool[tag].reqType = REQ_TYPE_NVME_DMA;
	reqPoolPtr->reqPool[tag].reqOpt.dataBufFormat = REQ_OPT_DATA_BUF_ENTRY;
	reqPoolPtr->reqPool[tag].dataBufInfo.entry = 3;
	reqPoolPtr->reqPool[tag].nvmeDmaInfo.nvmeBlockOffset = 2;
	TEST_ASSERT_EQUAL_UINT32(DATA_BUFFER_BASE_ADDR + 3 * BYTES_PER_DATA_REGION_OF_SLICE + 2 * BYTES_PER_NVME_BLOCK, GenerateDataBufAddr(tag));
	TEST_ASSERT_EQUAL_UINT32(SPARE_DATA_BUFFER_BASE_ADDR + 3 * BYTES_PER_SPARE_REGION_OF_SLICE, GenerateSpareDataBufAddr(tag));

	PutToFreeReqQ(tag);
}

/* ---- Row-address dependencies -------------------------------------------- */

static void test_read_of_unprogrammed_page_waits_until_page_is_written(void)
{
	unsigned int vsa = ftl_test_write_slice(0, 0x01);
	unsigned int die = Vsa2VdieTranslation(vsa);
	unsigned int ch = Vdie2PchTranslation(die), way = Vdie2PwayTranslation(die);
	unsigned int vblock = Vsa2VblockTranslation(vsa);
	unsigned int page = Vsa2VpageTranslation(vsa);
	unsigned int readTag;

	readTag = IssueRead(ftl_test_vsa(die, vblock, page + 1), REQ_OPT_NAND_ECC_WARNING_OFF);

	TEST_ASSERT_EQUAL_UINT32(1, blockedByRowAddrDepReqQ[ch][way].reqCnt);
	TEST_ASSERT_EQUAL_UINT32(1, rowAddrDependencyTablePtr->block[ch][way][vblock].blockedReadReqCnt);
	SchedulingNandReq();
	TEST_ASSERT_EQUAL_UINT32(1, blockedByRowAddrDepReqQ[ch][way].reqCnt);

	/* Programming page+1 unblocks the read. */
	IssueRawWrite(ftl_test_vsa(die, vblock, page + 1), 0x02);
	ftl_test_drain();

	TEST_ASSERT_EQUAL_UINT32(0, blockedByRowAddrDepReqQ[ch][way].reqCnt);
	TEST_ASSERT_EQUAL_UINT32(0, rowAddrDependencyTablePtr->block[ch][way][vblock].blockedReadReqCnt);
	TEST_ASSERT_EQUAL_UINT8(0x02, ((unsigned char *)host_memory_ptr(GenerateDataBufAddr(readTag)))[0]);
}

static void test_read_waits_for_blocked_erase_of_same_block_to_be_released(void)
{
	unsigned int vsa = ftl_test_write_slice(0, 0x10);
	unsigned int die = Vsa2VdieTranslation(vsa);
	unsigned int ch = Vdie2PchTranslation(die), way = Vdie2PwayTranslation(die);
	unsigned int vblock = Vsa2VblockTranslation(vsa);
	unsigned int eraseTag, readTag;

	/* Erase claims one more programmed page than the dependency table has permitted -> blocked. */
	eraseTag = NewNandReq(REQ_CODE_ERASE, ftl_test_vsa(die, vblock, 0), REQ_OPT_NAND_ECC_WARNING_OFF);
	reqPoolPtr->reqPool[eraseTag].nandInfo.programmedPageCnt = rowAddrDependencyTablePtr->block[ch][way][vblock].permittedProgPage + 1;
	SelectLowLevelReqQ(eraseTag);
	TEST_ASSERT_EQUAL_UINT32(1, rowAddrDependencyTablePtr->block[ch][way][vblock].blockedEraseReqFlag);
	TEST_ASSERT_EQUAL_UINT32(1, blockedByRowAddrDepReqQ[ch][way].reqCnt);

	/* Queue (but do not schedule) the write that makes the erase eligible. */
	IssueRawWrite(ftl_test_vsa(die, vblock, Vsa2VpageTranslation(vsa) + 1), 0x20);
	TEST_ASSERT_EQUAL_UINT32(reqPoolPtr->reqPool[eraseTag].nandInfo.programmedPageCnt, rowAddrDependencyTablePtr->block[ch][way][vblock].permittedProgPage);

	/* A read on the same block must synchronously let the erase go first. */
	readTag = IssueRead(ftl_test_vsa(die, vblock, 0), REQ_OPT_NAND_ECC_WARNING_OFF);
	TEST_ASSERT_EQUAL_UINT32(0, rowAddrDependencyTablePtr->block[ch][way][vblock].blockedEraseReqFlag);
	TEST_ASSERT_EQUAL_UINT32(0, rowAddrDependencyTablePtr->block[ch][way][vblock].permittedProgPage);
	/* Page 0 is no longer permitted after the erase, so the read is parked. */
	TEST_ASSERT_EQUAL_UINT32(1, rowAddrDependencyTablePtr->block[ch][way][vblock].blockedReadReqCnt);
	TEST_ASSERT_EQUAL_UINT32(REQ_QUEUE_TYPE_BLOCKED_BY_ROW_ADDR_DEP, reqPoolPtr->reqPool[readTag].reqQueueType);

	/* Programming page 0 again releases it. */
	IssueRawWrite(ftl_test_vsa(die, vblock, 0), 0x30);
	ftl_test_drain();

	TEST_ASSERT_EQUAL_UINT32(0, rowAddrDependencyTablePtr->block[ch][way][vblock].blockedReadReqCnt);
	TEST_ASSERT_EQUAL_UINT8(0x30, ((unsigned char *)host_memory_ptr(GenerateDataBufAddr(readTag)))[0]);
	TEST_ASSERT_GREATER_OR_EQUAL_UINT32(1, fake_nand_block_erase_count(ch, way, PhyBlockOfVsa(vsa)));
}

int main(void)
{
	UNITY_BEGIN();
	RUN_TEST(test_read_with_many_corrected_bits_completes_but_books_grown_bad_block);
	RUN_TEST(test_ecc_warning_is_ignored_when_request_does_not_ask_for_it);
	RUN_TEST(test_corrected_bits_at_threshold_do_not_trigger_warning);
	RUN_TEST(test_uncorrectable_read_is_retried_then_marks_block_bad);
	RUN_TEST(test_retry_budget_is_per_die_and_restored_after_failure);
	RUN_TEST(test_erase_failure_marks_physical_block_bad);
	RUN_TEST(test_row_addr_for_main_block_space_applies_bad_block_remap);
	RUN_TEST(test_row_addr_for_total_block_space_is_not_remapped);
	RUN_TEST(test_data_buf_addr_follows_request_format);
	RUN_TEST(test_read_of_unprogrammed_page_waits_until_page_is_written);
	RUN_TEST(test_read_waits_for_blocked_erase_of_same_block_to_be_released);
	return UNITY_END();
}
