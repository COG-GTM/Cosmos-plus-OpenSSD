/*
 * Defensive assert() paths in the request pipeline, plus the synchronous
 * erase-release path taken when a buffer-blocked read meets a blocked erase.
 *
 * Every "asserts" test runs the offending call in a forked child through
 * ftl_test_expect_abort() so the abort never takes the test runner down.
 */
#include <string.h>

#include "unity.h"

#include "ftl_test_env.h"

/* Out-of-range values that still fit the request bit-fields. */
#define BAD_NAND_ADDR 3u   /* reqOpt.nandAddr is 2 bits */
#define BAD_REQ_TYPE 0xfu  /* reqType is 4 bits */
#define BAD_OPTION 0x7fu   /* plain unsigned int arguments */
#define BAD_CODE 0x3fu

/* Internal to request_transform.c; exercised directly here. */
unsigned int CheckRowAddrDep(unsigned int reqSlotTag, unsigned int checkRowAddrDepOpt);
unsigned int UpdateRowAddrDepTableForBufBlockedReq(unsigned int reqSlotTag);

void setUp(void)
{
	ftl_test_env_init();
}

void tearDown(void)
{
}

static unsigned int NewNandReq(unsigned int reqCode, unsigned int vsa)
{
	unsigned int tag = GetFromFreeReqQ();

	reqPoolPtr->reqPool[tag].reqType = REQ_TYPE_NAND;
	reqPoolPtr->reqPool[tag].reqCode = reqCode;
	reqPoolPtr->reqPool[tag].logicalSliceAddr = LSA_NONE;
	reqPoolPtr->reqPool[tag].reqOpt.nandAddr = REQ_OPT_NAND_ADDR_VSA;
	reqPoolPtr->reqPool[tag].reqOpt.nandEcc = REQ_OPT_NAND_ECC_ON;
	reqPoolPtr->reqPool[tag].reqOpt.nandEccWarning = REQ_OPT_NAND_ECC_WARNING_OFF;
	reqPoolPtr->reqPool[tag].reqOpt.rowAddrDependencyCheck = REQ_OPT_ROW_ADDR_DEPENDENCY_CHECK;
	reqPoolPtr->reqPool[tag].reqOpt.blockSpace = REQ_OPT_BLOCK_SPACE_MAIN;
	reqPoolPtr->reqPool[tag].reqOpt.dataBufFormat = REQ_OPT_DATA_BUF_NONE;
	reqPoolPtr->reqPool[tag].nandInfo.virtualSliceAddr = vsa;
	return tag;
}

static void UseTempBuffer(unsigned int tag)
{
	unsigned int entry = AllocateTempDataBuf(Vsa2VdieTranslation(reqPoolPtr->reqPool[tag].nandInfo.virtualSliceAddr));
	reqPoolPtr->reqPool[tag].reqOpt.dataBufFormat = REQ_OPT_DATA_BUF_TEMP_ENTRY;
	reqPoolPtr->reqPool[tag].dataBufInfo.entry = entry;
	UpdateTempDataBufEntryInfoBlockingReq(entry, tag);
}

static unsigned int IssueRawWrite(unsigned int vsa, unsigned char fillByte)
{
	unsigned int tag = NewNandReq(REQ_CODE_WRITE, vsa);
	reqPoolPtr->reqPool[tag].reqOpt.dataBufFormat = REQ_OPT_DATA_BUF_ADDR;
	reqPoolPtr->reqPool[tag].dataBufInfo.addr = RESERVED_DATA_BUFFER_BASE_ADDR;
	memset(host_memory_ptr(GenerateDataBufAddr(tag)), fillByte, BYTES_PER_DATA_REGION_OF_SLICE);
	SelectLowLevelReqQ(tag);
	return tag;
}

/* ---- request_transform.c ------------------------------------------------- */

static void NvmeToSliceWithBadCommand(void)
{
	ReqTransNvmeToSlice(0, 0, 0, BAD_CODE);
}

static void test_nvme_to_slice_rejects_unknown_command_code(void)
{
	TEST_ASSERT_TRUE(ftl_test_expect_abort(NvmeToSliceWithBadCommand));
}

static void SliceToLowLevelWithBadCode(void)
{
	unsigned int tag = GetFromFreeReqQ();
	reqPoolPtr->reqPool[tag].reqType = REQ_TYPE_SLICE;
	reqPoolPtr->reqPool[tag].reqCode = REQ_CODE_ERASE;
	reqPoolPtr->reqPool[tag].logicalSliceAddr = 0;
	reqPoolPtr->reqPool[tag].nvmeDmaInfo.startIndex = 0;
	reqPoolPtr->reqPool[tag].nvmeDmaInfo.numOfNvmeBlock = NVME_BLOCKS_PER_SLICE;
	PutToSliceReqQ(tag);
	ReqTransSliceToLowLevel();
}

static void test_slice_to_low_level_rejects_non_io_request(void)
{
	TEST_ASSERT_TRUE(ftl_test_expect_abort(SliceToLowLevelWithBadCode));
}

static void test_slice_to_low_level_with_empty_queue_is_a_no_op(void)
{
	unsigned int freeBefore = ftl_test_count_free_reqs();
	ReqTransSliceToLowLevel();
	TEST_ASSERT_EQUAL_UINT32(freeBefore, ftl_test_count_free_reqs());
}

static void SelectWithBadNandAddr(void)
{
	unsigned int tag = NewNandReq(REQ_CODE_READ, ftl_test_vsa(0, 0, 0));
	reqPoolPtr->reqPool[tag].reqOpt.nandAddr = BAD_NAND_ADDR;
	SelectLowLevelReqQ(tag);
}

static void test_select_low_level_rejects_unknown_nand_address_mode(void)
{
	TEST_ASSERT_TRUE(ftl_test_expect_abort(SelectWithBadNandAddr));
}

static void SelectWithBadReqType(void)
{
	unsigned int tag = NewNandReq(REQ_CODE_READ, ftl_test_vsa(0, 0, 0));
	reqPoolPtr->reqPool[tag].reqType = BAD_REQ_TYPE;
	SelectLowLevelReqQ(tag);
}

static void test_select_low_level_rejects_unknown_request_type(void)
{
	TEST_ASSERT_TRUE(ftl_test_expect_abort(SelectWithBadReqType));
}

static void RowAddrDepWithBadNandAddr(void)
{
	unsigned int tag = NewNandReq(REQ_CODE_READ, ftl_test_vsa(0, 0, 0));
	reqPoolPtr->reqPool[tag].reqOpt.nandAddr = BAD_NAND_ADDR;
	CheckRowAddrDep(tag, ROW_ADDR_DEPENDENCY_CHECK_OPT_SELECT);
}

static void RowAddrDepReadWithBadOption(void)
{
	CheckRowAddrDep(NewNandReq(REQ_CODE_READ, ftl_test_vsa(0, 0, 0)), BAD_OPTION);
}

static void RowAddrDepEraseWithBadOption(void)
{
	unsigned int tag = NewNandReq(REQ_CODE_ERASE, ftl_test_vsa(0, 0, 0));
	reqPoolPtr->reqPool[tag].nandInfo.programmedPageCnt = ROWS_PER_MLC_BLOCK;
	CheckRowAddrDep(tag, BAD_OPTION);
}

static void RowAddrDepWithBadCode(void)
{
	CheckRowAddrDep(NewNandReq(REQ_CODE_RESET, ftl_test_vsa(0, 0, 0)), ROW_ADDR_DEPENDENCY_CHECK_OPT_SELECT);
}

static void test_check_row_addr_dep_rejects_bad_inputs(void)
{
	TEST_ASSERT_TRUE(ftl_test_expect_abort(RowAddrDepWithBadNandAddr));
	TEST_ASSERT_TRUE(ftl_test_expect_abort(RowAddrDepReadWithBadOption));
	TEST_ASSERT_TRUE(ftl_test_expect_abort(RowAddrDepEraseWithBadOption));
	TEST_ASSERT_TRUE(ftl_test_expect_abort(RowAddrDepWithBadCode));
}

static void test_check_row_addr_dep_release_on_ineligible_erase_stays_blocked(void)
{
	unsigned int tag = NewNandReq(REQ_CODE_ERASE, ftl_test_vsa(0, 0, 0));
	reqPoolPtr->reqPool[tag].nandInfo.programmedPageCnt = ROWS_PER_MLC_BLOCK;

	TEST_ASSERT_EQUAL_UINT32(ROW_ADDR_DEPENDENCY_REPORT_BLOCKED, CheckRowAddrDep(tag, ROW_ADDR_DEPENDENCY_CHECK_OPT_RELEASE));
	TEST_ASSERT_EQUAL_UINT32(0, rowAddrDependencyTablePtr->block[0][0][0].blockedEraseReqFlag);
}

static void BufBlockedUpdateWithBadNandAddr(void)
{
	unsigned int tag = NewNandReq(REQ_CODE_READ, ftl_test_vsa(0, 0, 0));
	reqPoolPtr->reqPool[tag].reqOpt.nandAddr = BAD_NAND_ADDR;
	UpdateRowAddrDepTableForBufBlockedReq(tag);
}

static void test_buf_blocked_dependency_update_rejects_unknown_nand_address_mode(void)
{
	TEST_ASSERT_TRUE(ftl_test_expect_abort(BufBlockedUpdateWithBadNandAddr));
}

static void IssueDmaWithBadCode(void)
{
	unsigned int tag = GetFromFreeReqQ();
	reqPoolPtr->reqPool[tag].reqType = REQ_TYPE_NVME_DMA;
	reqPoolPtr->reqPool[tag].reqCode = REQ_CODE_ERASE;
	reqPoolPtr->reqPool[tag].reqOpt.dataBufFormat = REQ_OPT_DATA_BUF_ENTRY;
	reqPoolPtr->reqPool[tag].dataBufInfo.entry = 0;
	reqPoolPtr->reqPool[tag].nvmeDmaInfo.startIndex = 0;
	reqPoolPtr->reqPool[tag].nvmeDmaInfo.numOfNvmeBlock = 1;
	IssueNvmeDmaReq(tag);
}

static void test_issue_nvme_dma_rejects_non_dma_code(void)
{
	TEST_ASSERT_TRUE(ftl_test_expect_abort(IssueDmaWithBadCode));
}

/* ---- Buffer-blocked read meeting a blocked erase ------------------------- */

static void test_buf_blocked_read_synchronously_releases_blocked_erase(void)
{
	unsigned int vsa = ftl_test_write_slice(0, 0x10);
	unsigned int die = Vsa2VdieTranslation(vsa);
	unsigned int ch = Vdie2PchTranslation(die), way = Vdie2PwayTranslation(die);
	unsigned int vblock = Vsa2VblockTranslation(vsa);
	unsigned int page = Vsa2VpageTranslation(vsa);
	unsigned int eraseTag, holderTag, readTag;

	/* Erase blocked: it claims one more programmed page than has been permitted. */
	eraseTag = NewNandReq(REQ_CODE_ERASE, ftl_test_vsa(die, vblock, 0));
	reqPoolPtr->reqPool[eraseTag].nandInfo.programmedPageCnt = page + 2;
	SelectLowLevelReqQ(eraseTag);
	TEST_ASSERT_EQUAL_UINT32(1, rowAddrDependencyTablePtr->block[ch][way][vblock].blockedEraseReqFlag);

	/* The write that makes the erase eligible sits in the NAND queue. */
	IssueRawWrite(ftl_test_vsa(die, vblock, page + 1), 0x20);

	/* A dependency-free read pins the die's temporary buffer. */
	holderTag = NewNandReq(REQ_CODE_READ, vsa);
	reqPoolPtr->reqPool[holderTag].reqOpt.rowAddrDependencyCheck = REQ_OPT_ROW_ADDR_DEPENDENCY_NONE;
	UseTempBuffer(holderTag);
	SelectLowLevelReqQ(holderTag);

	/* This read is buffer-blocked behind holderTag and must let the erase go first. */
	readTag = NewNandReq(REQ_CODE_READ, ftl_test_vsa(die, vblock, 0));
	UseTempBuffer(readTag);
	SelectLowLevelReqQ(readTag);

	TEST_ASSERT_EQUAL_UINT32(0, rowAddrDependencyTablePtr->block[ch][way][vblock].blockedEraseReqFlag);
	TEST_ASSERT_EQUAL_UINT32(0, rowAddrDependencyTablePtr->block[ch][way][vblock].permittedProgPage);
	TEST_ASSERT_EQUAL_UINT32(REQ_QUEUE_TYPE_BLOCKED_BY_ROW_ADDR_DEP, reqPoolPtr->reqPool[readTag].reqQueueType);
	TEST_ASSERT_EQUAL_UINT32(1, rowAddrDependencyTablePtr->block[ch][way][vblock].blockedReadReqCnt);
	TEST_ASSERT_EQUAL_UINT32(0, blockedByBufDepReqQ.reqCnt);

	IssueRawWrite(ftl_test_vsa(die, vblock, 0), 0x30);
	ftl_test_drain();

	TEST_ASSERT_EQUAL_UINT32(0, rowAddrDependencyTablePtr->block[ch][way][vblock].blockedReadReqCnt);
	TEST_ASSERT_EQUAL_UINT8(0x30, ((unsigned char *)host_memory_ptr(GenerateDataBufAddr(readTag)))[0]);
}

static void test_buf_blocked_erase_flags_block_and_runs_after_buffer_frees(void)
{
	unsigned int vsa = ftl_test_write_slice(0, 0x40);
	unsigned int die = Vsa2VdieTranslation(vsa);
	unsigned int ch = Vdie2PchTranslation(die), way = Vdie2PwayTranslation(die);
	unsigned int vblock = Vsa2VblockTranslation(vsa);
	unsigned int holderTag, eraseTag;
	FAKE_NAND_DIE_STATS before = fake_nand_stats(ch, way);

	holderTag = NewNandReq(REQ_CODE_READ, vsa);
	reqPoolPtr->reqPool[holderTag].reqOpt.rowAddrDependencyCheck = REQ_OPT_ROW_ADDR_DEPENDENCY_NONE;
	UseTempBuffer(holderTag);
	SelectLowLevelReqQ(holderTag);

	eraseTag = NewNandReq(REQ_CODE_ERASE, ftl_test_vsa(die, vblock, 0));
	reqPoolPtr->reqPool[eraseTag].nandInfo.programmedPageCnt = rowAddrDependencyTablePtr->block[ch][way][vblock].permittedProgPage;
	UseTempBuffer(eraseTag);
	SelectLowLevelReqQ(eraseTag);

	TEST_ASSERT_EQUAL_UINT32(REQ_QUEUE_TYPE_BLOCKED_BY_BUF_DEP, reqPoolPtr->reqPool[eraseTag].reqQueueType);
	TEST_ASSERT_EQUAL_UINT32(1, rowAddrDependencyTablePtr->block[ch][way][vblock].blockedEraseReqFlag);

	ftl_test_drain();

	TEST_ASSERT_EQUAL_UINT32(0, rowAddrDependencyTablePtr->block[ch][way][vblock].blockedEraseReqFlag);
	TEST_ASSERT_EQUAL_UINT32(0, rowAddrDependencyTablePtr->block[ch][way][vblock].permittedProgPage);
	TEST_ASSERT_EQUAL_UINT32(1, fake_nand_stats(ch, way).erases - before.erases);
}

/* ---- request_schedule.c -------------------------------------------------- */

static void RowAddrWithBadNandAddr(void)
{
	unsigned int tag = NewNandReq(REQ_CODE_READ, ftl_test_vsa(0, 0, 0));
	reqPoolPtr->reqPool[tag].reqOpt.nandAddr = BAD_NAND_ADDR;
	GenerateNandRowAddr(tag);
}

static void test_generate_row_addr_rejects_unknown_nand_address_mode(void)
{
	TEST_ASSERT_TRUE(ftl_test_expect_abort(RowAddrWithBadNandAddr));
}

static void DmaDataBufAddrWithBadFormat(void)
{
	unsigned int tag = GetFromFreeReqQ();
	reqPoolPtr->reqPool[tag].reqType = REQ_TYPE_NVME_DMA;
	reqPoolPtr->reqPool[tag].reqOpt.dataBufFormat = REQ_OPT_DATA_BUF_ADDR;
	GenerateDataBufAddr(tag);
}

static void DmaSpareBufAddrWithBadFormat(void)
{
	unsigned int tag = GetFromFreeReqQ();
	reqPoolPtr->reqPool[tag].reqType = REQ_TYPE_NVME_DMA;
	reqPoolPtr->reqPool[tag].reqOpt.dataBufFormat = REQ_OPT_DATA_BUF_ADDR;
	GenerateSpareDataBufAddr(tag);
}

static void test_generate_buf_addr_rejects_absolute_address_for_dma(void)
{
	TEST_ASSERT_TRUE(ftl_test_expect_abort(DmaDataBufAddrWithBadFormat));
	TEST_ASSERT_TRUE(ftl_test_expect_abort(DmaSpareBufAddrWithBadFormat));
}

static void IssueUnknownNandReq(void)
{
	unsigned int tag = NewNandReq(REQ_CODE_FLUSH, ftl_test_vsa(0, 0, 0));
	reqPoolPtr->reqPool[tag].reqOpt.rowAddrDependencyCheck = REQ_OPT_ROW_ADDR_DEPENDENCY_NONE;
	SelectLowLevelReqQ(tag);
	ftl_test_drain();
}

static void test_scheduler_rejects_unknown_nand_request_code(void)
{
	TEST_ASSERT_TRUE(ftl_test_expect_abort(IssueUnknownNandReq));
}

static void test_reset_and_set_feature_requests_complete_without_nand_traffic(void)
{
	unsigned int die = 0, ch = Vdie2PchTranslation(die), way = Vdie2PwayTranslation(die);
	FAKE_NAND_DIE_STATS before = fake_nand_stats(ch, way), after;
	unsigned int tag;

	tag = NewNandReq(REQ_CODE_RESET, ftl_test_vsa(die, 0, 0));
	reqPoolPtr->reqPool[tag].reqOpt.rowAddrDependencyCheck = REQ_OPT_ROW_ADDR_DEPENDENCY_NONE;
	SelectLowLevelReqQ(tag);
	tag = NewNandReq(REQ_CODE_SET_FEATURE, ftl_test_vsa(die, 0, 0));
	reqPoolPtr->reqPool[tag].reqOpt.rowAddrDependencyCheck = REQ_OPT_ROW_ADDR_DEPENDENCY_NONE;
	SelectLowLevelReqQ(tag);
	ftl_test_drain();

	after = fake_nand_stats(ch, way);
	TEST_ASSERT_EQUAL_UINT32(before.programs, after.programs);
	TEST_ASSERT_EQUAL_UINT32(before.erases, after.erases);
	TEST_ASSERT_EQUAL_UINT32(before.readTriggers, after.readTriggers);
	TEST_ASSERT_EQUAL_UINT32(0, ftl_test_pending_nand_reqs());
}

/* ---- request_allocation.c ------------------------------------------------ */

static void GetFromEmptyNandReqQ(void)
{
	GetFromNandReqQ(0, 0, REQ_STATUS_DONE, REQ_CODE_READ);
}

static void RemoveNoneFromBufDepQ(void)
{
	SelectiveGetFromBlockedByBufDepReqQ(REQ_SLOT_TAG_NONE);
}

static void RemoveNoneFromRowAddrDepQ(void)
{
	SelectiveGetFromBlockedByRowAddrDepReqQ(REQ_SLOT_TAG_NONE, 0, 0);
}

static void test_queue_helpers_reject_empty_queue_and_none_tag(void)
{
	TEST_ASSERT_TRUE(ftl_test_expect_abort(GetFromEmptyNandReqQ));
	TEST_ASSERT_TRUE(ftl_test_expect_abort(RemoveNoneFromBufDepQ));
	TEST_ASSERT_TRUE(ftl_test_expect_abort(RemoveNoneFromRowAddrDepQ));
}

static void test_removing_middle_and_tail_of_blocked_queues_relinks_them(void)
{
	unsigned int a = GetFromFreeReqQ(), b = GetFromFreeReqQ(), c = GetFromFreeReqQ();

	PutToBlockedByBufDepReqQ(a);
	PutToBlockedByBufDepReqQ(b);
	PutToBlockedByBufDepReqQ(c);
	SelectiveGetFromBlockedByBufDepReqQ(b);
	TEST_ASSERT_EQUAL_UINT32(c, reqPoolPtr->reqPool[a].nextReq);
	TEST_ASSERT_EQUAL_UINT32(a, reqPoolPtr->reqPool[c].prevReq);
	SelectiveGetFromBlockedByBufDepReqQ(c);
	TEST_ASSERT_EQUAL_UINT32(a, blockedByBufDepReqQ.tailReq);
	TEST_ASSERT_EQUAL_UINT32(REQ_SLOT_TAG_NONE, reqPoolPtr->reqPool[a].nextReq);
	SelectiveGetFromBlockedByBufDepReqQ(a);
	TEST_ASSERT_EQUAL_UINT32(0, blockedByBufDepReqQ.reqCnt);

	PutToBlockedByRowAddrDepReqQ(a, 0, 0);
	PutToBlockedByRowAddrDepReqQ(b, 0, 0);
	PutToBlockedByRowAddrDepReqQ(c, 0, 0);
	SelectiveGetFromBlockedByRowAddrDepReqQ(b, 0, 0);
	TEST_ASSERT_EQUAL_UINT32(c, reqPoolPtr->reqPool[a].nextReq);
	SelectiveGetFromBlockedByRowAddrDepReqQ(c, 0, 0);
	TEST_ASSERT_EQUAL_UINT32(a, blockedByRowAddrDepReqQ[0][0].tailReq);
	SelectiveGetFromBlockedByRowAddrDepReqQ(a, 0, 0);
	TEST_ASSERT_EQUAL_UINT32(0, blockedByRowAddrDepReqQ[0][0].reqCnt);

	PutToFreeReqQ(a);
	PutToFreeReqQ(b);
	PutToFreeReqQ(c);
}

int main(void)
{
	UNITY_BEGIN();
	RUN_TEST(test_nvme_to_slice_rejects_unknown_command_code);
	RUN_TEST(test_slice_to_low_level_rejects_non_io_request);
	RUN_TEST(test_slice_to_low_level_with_empty_queue_is_a_no_op);
	RUN_TEST(test_select_low_level_rejects_unknown_nand_address_mode);
	RUN_TEST(test_select_low_level_rejects_unknown_request_type);
	RUN_TEST(test_check_row_addr_dep_rejects_bad_inputs);
	RUN_TEST(test_check_row_addr_dep_release_on_ineligible_erase_stays_blocked);
	RUN_TEST(test_buf_blocked_dependency_update_rejects_unknown_nand_address_mode);
	RUN_TEST(test_issue_nvme_dma_rejects_non_dma_code);
	RUN_TEST(test_buf_blocked_read_synchronously_releases_blocked_erase);
	RUN_TEST(test_buf_blocked_erase_flags_block_and_runs_after_buffer_frees);
	RUN_TEST(test_generate_row_addr_rejects_unknown_nand_address_mode);
	RUN_TEST(test_generate_buf_addr_rejects_absolute_address_for_dma);
	RUN_TEST(test_scheduler_rejects_unknown_nand_request_code);
	RUN_TEST(test_reset_and_set_feature_requests_complete_without_nand_traffic);
	RUN_TEST(test_queue_helpers_reject_empty_queue_and_none_tag);
	RUN_TEST(test_removing_middle_and_tail_of_blocked_queues_relinks_them);
	return UNITY_END();
}
