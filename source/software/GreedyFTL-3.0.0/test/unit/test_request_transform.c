/* Unit tests for request_transform.c. */
#include "unity.h"

#include "fw_test.h"
#include "memory_map.h"

#define CMD_SLOT 5
#define TEST_DIE 0
#define TEST_BLOCK 3

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

static unsigned int make_nand_req(unsigned int reqCode, unsigned int blockNo,
		unsigned int pageNo)
{
	unsigned int reqSlotTag = GetFromFreeReqQ();

	req(reqSlotTag)->reqType = REQ_TYPE_NAND;
	req(reqSlotTag)->reqCode = reqCode;
	req(reqSlotTag)->reqOpt.nandAddr = REQ_OPT_NAND_ADDR_VSA;
	req(reqSlotTag)->reqOpt.dataBufFormat = REQ_OPT_DATA_BUF_NONE;
	req(reqSlotTag)->reqOpt.rowAddrDependencyCheck = REQ_OPT_ROW_ADDR_DEPENDENCY_CHECK;
	req(reqSlotTag)->reqOpt.blockSpace = REQ_OPT_BLOCK_SPACE_MAIN;
	req(reqSlotTag)->nandInfo.virtualSliceAddr = Vorg2VsaTranslation(TEST_DIE, blockNo, pageNo);
	req(reqSlotTag)->nandInfo.programmedPageCnt = 0;
	req(reqSlotTag)->prevBlockingReq = REQ_SLOT_TAG_NONE;
	req(reqSlotTag)->nextBlockingReq = REQ_SLOT_TAG_NONE;
	return reqSlotTag;
}

static ROW_ADDR_DEPENDENCY_ENTRY *dep_entry(unsigned int blockNo)
{
	unsigned int chNo = Vdie2PchTranslation(TEST_DIE);
	unsigned int wayNo = Vdie2PwayTranslation(TEST_DIE);

	return &rowAddrDependencyTablePtr->block[chNo][wayNo][blockNo];
}

/* Marks every page of the block holding vsa as programmed so reads pass. */
static void permit_reads_of(unsigned int vsa)
{
	unsigned int dieNo = Vsa2VdieTranslation(vsa);
	unsigned int chNo = Vdie2PchTranslation(dieNo);
	unsigned int wayNo = Vdie2PwayTranslation(dieNo);
	unsigned int blockNo = Vsa2VblockTranslation(vsa);

	rowAddrDependencyTablePtr->block[chNo][wayNo][blockNo].permittedProgPage = USER_PAGES_PER_BLOCK;
}

/* ---- ReqTransNvmeToSlice ---- */

static void test_smoke_single_slice_read_becomes_one_slice_request(void)
{
	/* nlb is zero based: 0 means one 4 KiB NVMe block. */
	ReqTransNvmeToSlice(0, 0, 0, IO_NVM_READ);

	TEST_ASSERT_EQUAL_UINT(1, sliceReqQ.reqCnt);
	TEST_ASSERT_EQUAL_UINT(REQ_CODE_READ, req(sliceReqQ.headReq)->reqCode);
	TEST_ASSERT_EQUAL_UINT(0, req(sliceReqQ.headReq)->logicalSliceAddr);
}

static void test_smoke_two_slice_write_is_split(void)
{
	ReqTransNvmeToSlice(0, 0, 2 * NVME_BLOCKS_PER_SLICE - 1, IO_NVM_WRITE);

	TEST_ASSERT_EQUAL_UINT(2, sliceReqQ.reqCnt);
}

static void test_unaligned_range_spanning_three_slices_sets_dma_windows(void)
{
	const unsigned int startLba = NVME_BLOCKS_PER_SLICE + 1;
	const unsigned int nlb = 2 * NVME_BLOCKS_PER_SLICE; /* 2 slices + 1 block */
	unsigned int first, middle, last;

	ReqTransNvmeToSlice(CMD_SLOT, startLba, nlb, IO_NVM_WRITE);

	TEST_ASSERT_EQUAL_UINT(3, sliceReqQ.reqCnt);
	first = sliceReqQ.headReq;
	middle = req(first)->nextReq;
	last = req(middle)->nextReq;
	TEST_ASSERT_EQUAL_UINT(sliceReqQ.tailReq, last);

	TEST_ASSERT_EQUAL_UINT(1, req(first)->logicalSliceAddr);
	TEST_ASSERT_EQUAL_UINT(1, req(first)->nvmeDmaInfo.nvmeBlockOffset);
	TEST_ASSERT_EQUAL_UINT(0, req(first)->nvmeDmaInfo.startIndex);
	TEST_ASSERT_EQUAL_UINT(NVME_BLOCKS_PER_SLICE - 1, req(first)->nvmeDmaInfo.numOfNvmeBlock);
	TEST_ASSERT_EQUAL_UINT(CMD_SLOT, req(first)->nvmeCmdSlotTag);
	TEST_ASSERT_EQUAL_UINT(REQ_TYPE_SLICE, req(first)->reqType);

	TEST_ASSERT_EQUAL_UINT(2, req(middle)->logicalSliceAddr);
	TEST_ASSERT_EQUAL_UINT(0, req(middle)->nvmeDmaInfo.nvmeBlockOffset);
	TEST_ASSERT_EQUAL_UINT(NVME_BLOCKS_PER_SLICE - 1, req(middle)->nvmeDmaInfo.startIndex);
	TEST_ASSERT_EQUAL_UINT(NVME_BLOCKS_PER_SLICE, req(middle)->nvmeDmaInfo.numOfNvmeBlock);

	TEST_ASSERT_EQUAL_UINT(3, req(last)->logicalSliceAddr);
	TEST_ASSERT_EQUAL_UINT(0, req(last)->nvmeDmaInfo.nvmeBlockOffset);
	TEST_ASSERT_EQUAL_UINT(2 * NVME_BLOCKS_PER_SLICE - 1, req(last)->nvmeDmaInfo.startIndex);
	TEST_ASSERT_EQUAL_UINT(2, req(last)->nvmeDmaInfo.numOfNvmeBlock);
}

static void test_range_ending_on_slice_boundary_has_no_tail_request(void)
{
	ReqTransNvmeToSlice(CMD_SLOT, 1, 2 * NVME_BLOCKS_PER_SLICE - 2, IO_NVM_READ);

	TEST_ASSERT_EQUAL_UINT(2, sliceReqQ.reqCnt);
	TEST_ASSERT_EQUAL_UINT(NVME_BLOCKS_PER_SLICE - 1,
			req(sliceReqQ.headReq)->nvmeDmaInfo.numOfNvmeBlock);
	TEST_ASSERT_EQUAL_UINT(NVME_BLOCKS_PER_SLICE,
			req(sliceReqQ.tailReq)->nvmeDmaInfo.numOfNvmeBlock);
}

static void test_unsupported_command_code_asserts(void)
{
	FW_EXPECT_ASSERT(ReqTransNvmeToSlice(0, 0, 0, IO_NVM_FLUSH));
}

/* ---- ReqTransSliceToLowLevel ---- */

static void test_read_of_unwritten_slice_skips_nand_and_issues_tx_dma(void)
{
	unsigned int reqSlotTag;

	ReqTransNvmeToSlice(CMD_SLOT, 0, NVME_BLOCKS_PER_SLICE - 1, IO_NVM_READ);
	ReqTransSliceToLowLevel();

	TEST_ASSERT_EQUAL_UINT(0, sliceReqQ.reqCnt);
	TEST_ASSERT_EQUAL_UINT(1, nvmeDmaReqQ.reqCnt);
	TEST_ASSERT_EQUAL_UINT(0, notCompletedNandReqCnt);
	reqSlotTag = nvmeDmaReqQ.headReq;
	TEST_ASSERT_EQUAL_UINT(REQ_TYPE_NVME_DMA, req(reqSlotTag)->reqType);
	TEST_ASSERT_EQUAL_UINT(REQ_CODE_TxDMA, req(reqSlotTag)->reqCode);
	TEST_ASSERT_EQUAL_UINT(NVME_BLOCKS_PER_SLICE, mock_host_count(MOCK_HOST_SET_AUTO_TX_DMA));
	TEST_ASSERT_EQUAL_UINT(CMD_SLOT, mock_host_last(MOCK_HOST_SET_AUTO_TX_DMA)->args[0]);
	TEST_ASSERT_EQUAL_UINT(DATA_BUF_CLEAN,
			dataBufMapPtr->dataBuf[req(reqSlotTag)->dataBufInfo.entry].dirty);
}

static void test_full_slice_write_issues_rx_dma_and_marks_buffer_dirty(void)
{
	unsigned int reqSlotTag;

	ReqTransNvmeToSlice(CMD_SLOT, NVME_BLOCKS_PER_SLICE, NVME_BLOCKS_PER_SLICE - 1, IO_NVM_WRITE);
	ReqTransSliceToLowLevel();

	TEST_ASSERT_EQUAL_UINT(1, nvmeDmaReqQ.reqCnt);
	TEST_ASSERT_EQUAL_UINT(0, notCompletedNandReqCnt);
	reqSlotTag = nvmeDmaReqQ.headReq;
	TEST_ASSERT_EQUAL_UINT(REQ_CODE_RxDMA, req(reqSlotTag)->reqCode);
	TEST_ASSERT_EQUAL_UINT(NVME_BLOCKS_PER_SLICE, mock_host_count(MOCK_HOST_SET_AUTO_RX_DMA));
	TEST_ASSERT_EQUAL_UINT(DATA_BUF_DIRTY,
			dataBufMapPtr->dataBuf[req(reqSlotTag)->dataBufInfo.entry].dirty);
	TEST_ASSERT_EQUAL_UINT(1,
			dataBufMapPtr->dataBuf[req(reqSlotTag)->dataBufInfo.entry].logicalSliceAddr);
}

static void test_partial_write_of_written_slice_reads_nand_first(void)
{
	unsigned int dmaReq;

	/* Make LSA 0 resident on NAND so the read-modify-write has something to fetch. */
	permit_reads_of(AddrTransWrite(0));

	ReqTransNvmeToSlice(CMD_SLOT, 0, 0, IO_NVM_WRITE);
	ReqTransSliceToLowLevel();

	/* NAND read queued first; the RxDMA is blocked behind it on the buffer. */
	TEST_ASSERT_EQUAL_UINT(1, notCompletedNandReqCnt);
	TEST_ASSERT_EQUAL_UINT(1, blockedByBufDepReqQ.reqCnt);
	TEST_ASSERT_EQUAL_UINT(0, nvmeDmaReqQ.reqCnt);
	dmaReq = blockedByBufDepReqQ.headReq;
	TEST_ASSERT_EQUAL_UINT(REQ_CODE_RxDMA, req(dmaReq)->reqCode);
	TEST_ASSERT_NOT_EQUAL(REQ_SLOT_TAG_NONE, req(dmaReq)->prevBlockingReq);

	/* Completing the NAND read releases the blocked DMA. */
	SyncAllLowLevelReqDone();

	/* The released DMA is issued and, with the mock reporting done, retired. */
	TEST_ASSERT_EQUAL_UINT(1, mock_nsc_count_cmd(V2FCommand_ReadPageTrigger));
	TEST_ASSERT_EQUAL_UINT(0, notCompletedNandReqCnt);
	TEST_ASSERT_EQUAL_UINT(0, blockedByBufDepReqQ.reqCnt);
	TEST_ASSERT_EQUAL_UINT(1, mock_host_count(MOCK_HOST_SET_AUTO_RX_DMA));
	TEST_ASSERT_EQUAL_UINT(0, nvmeDmaReqQ.reqCnt);
	TEST_ASSERT_EQUAL_UINT(REQ_QUEUE_TYPE_FREE, req(dmaReq)->reqQueueType);
}

static void test_read_of_written_slice_reads_nand_then_tx_dma(void)
{
	permit_reads_of(AddrTransWrite(7));

	ReqTransNvmeToSlice(CMD_SLOT, 7 * NVME_BLOCKS_PER_SLICE, 0, IO_NVM_READ);
	ReqTransSliceToLowLevel();
	TEST_ASSERT_EQUAL_UINT(1, notCompletedNandReqCnt);
	TEST_ASSERT_EQUAL_UINT(0, mock_host_count(MOCK_HOST_SET_AUTO_TX_DMA));

	SyncAllLowLevelReqDone();
	TEST_ASSERT_EQUAL_UINT(1, mock_nsc_count_cmd(V2FCommand_ReadPageTrigger));
	TEST_ASSERT_EQUAL_UINT(1, mock_host_count(MOCK_HOST_SET_AUTO_TX_DMA));
	TEST_ASSERT_EQUAL_UINT(0, nvmeDmaReqQ.reqCnt);
}

static void test_second_access_to_same_slice_hits_data_buffer(void)
{
	unsigned int firstEntry, secondEntry;

	ReqTransNvmeToSlice(CMD_SLOT, 0, NVME_BLOCKS_PER_SLICE - 1, IO_NVM_WRITE);
	ReqTransSliceToLowLevel();
	firstEntry = req(nvmeDmaReqQ.headReq)->dataBufInfo.entry;
	CheckDoneNvmeDmaReq();
	TEST_ASSERT_EQUAL_UINT(0, nvmeDmaReqQ.reqCnt);

	ReqTransNvmeToSlice(CMD_SLOT, 0, 0, IO_NVM_READ);
	ReqTransSliceToLowLevel();
	secondEntry = req(nvmeDmaReqQ.headReq)->dataBufInfo.entry;

	TEST_ASSERT_EQUAL_UINT(firstEntry, secondEntry);
	TEST_ASSERT_EQUAL_UINT(0, notCompletedNandReqCnt);
	TEST_ASSERT_EQUAL_UINT(1, mock_host_count(MOCK_HOST_SET_AUTO_TX_DMA));
}

static void test_evicting_dirty_buffer_writes_it_back_to_nand(void)
{
	unsigned int lsa;

	for (lsa = 0; lsa < AVAILABLE_DATA_BUFFER_ENTRY_COUNT; lsa++) {
		ReqTransNvmeToSlice(CMD_SLOT, lsa * NVME_BLOCKS_PER_SLICE,
				NVME_BLOCKS_PER_SLICE - 1, IO_NVM_WRITE);
		ReqTransSliceToLowLevel();
		CheckDoneNvmeDmaReq();
	}
	TEST_ASSERT_EQUAL_UINT(0, notCompletedNandReqCnt);
	TEST_ASSERT_EQUAL_UINT(0, mock_nsc_call_count());

	/* One more distinct slice forces eviction of LSA 0 (LRU tail). */
	ReqTransNvmeToSlice(CMD_SLOT, lsa * NVME_BLOCKS_PER_SLICE,
			NVME_BLOCKS_PER_SLICE - 1, IO_NVM_WRITE);
	ReqTransSliceToLowLevel();

	TEST_ASSERT_EQUAL_UINT(1, notCompletedNandReqCnt);
	TEST_ASSERT_NOT_EQUAL(VSA_FAIL, AddrTransRead(0));

	SyncAllLowLevelReqDone();
	TEST_ASSERT_EQUAL_UINT(1, mock_nsc_count_cmd(V2FCommand_ProgramPage));
	TEST_ASSERT_EQUAL_UINT(0, notCompletedNandReqCnt);
}

/* ---- CheckRowAddrDep ---- */

static void test_row_dep_write_passes_only_at_permitted_page(void)
{
	unsigned int page0 = make_nand_req(REQ_CODE_WRITE, TEST_BLOCK, 0);
	unsigned int page2 = make_nand_req(REQ_CODE_WRITE, TEST_BLOCK, 2);

	TEST_ASSERT_EQUAL_UINT(ROW_ADDR_DEPENDENCY_REPORT_PASS,
			CheckRowAddrDep(page0, ROW_ADDR_DEPENDENCY_CHECK_OPT_SELECT));
	TEST_ASSERT_EQUAL_UINT(1, dep_entry(TEST_BLOCK)->permittedProgPage);
	TEST_ASSERT_EQUAL_UINT(ROW_ADDR_DEPENDENCY_REPORT_BLOCKED,
			CheckRowAddrDep(page2, ROW_ADDR_DEPENDENCY_CHECK_OPT_SELECT));
	TEST_ASSERT_EQUAL_UINT(1, dep_entry(TEST_BLOCK)->permittedProgPage);
}

static void test_row_dep_read_blocks_until_page_programmed(void)
{
	unsigned int read = make_nand_req(REQ_CODE_READ, TEST_BLOCK, 0);

	TEST_ASSERT_EQUAL_UINT(ROW_ADDR_DEPENDENCY_REPORT_BLOCKED,
			CheckRowAddrDep(read, ROW_ADDR_DEPENDENCY_CHECK_OPT_SELECT));
	TEST_ASSERT_EQUAL_UINT(1, dep_entry(TEST_BLOCK)->blockedReadReqCnt);

	TEST_ASSERT_EQUAL_UINT(ROW_ADDR_DEPENDENCY_REPORT_BLOCKED,
			CheckRowAddrDep(read, ROW_ADDR_DEPENDENCY_CHECK_OPT_RELEASE));

	dep_entry(TEST_BLOCK)->permittedProgPage = 1;
	TEST_ASSERT_EQUAL_UINT(ROW_ADDR_DEPENDENCY_REPORT_PASS,
			CheckRowAddrDep(read, ROW_ADDR_DEPENDENCY_CHECK_OPT_RELEASE));
	TEST_ASSERT_EQUAL_UINT(0, dep_entry(TEST_BLOCK)->blockedReadReqCnt);

	TEST_ASSERT_EQUAL_UINT(ROW_ADDR_DEPENDENCY_REPORT_PASS,
			CheckRowAddrDep(read, ROW_ADDR_DEPENDENCY_CHECK_OPT_SELECT));
}

static void test_row_dep_erase_waits_for_programmed_pages_and_blocked_reads(void)
{
	unsigned int erase = make_nand_req(REQ_CODE_ERASE, TEST_BLOCK, 0);

	req(erase)->nandInfo.programmedPageCnt = 2;

	TEST_ASSERT_EQUAL_UINT(ROW_ADDR_DEPENDENCY_REPORT_BLOCKED,
			CheckRowAddrDep(erase, ROW_ADDR_DEPENDENCY_CHECK_OPT_SELECT));
	TEST_ASSERT_EQUAL_UINT(1, dep_entry(TEST_BLOCK)->blockedEraseReqFlag);

	dep_entry(TEST_BLOCK)->permittedProgPage = 2;
	dep_entry(TEST_BLOCK)->blockedReadReqCnt = 1;
	TEST_ASSERT_EQUAL_UINT(ROW_ADDR_DEPENDENCY_REPORT_BLOCKED,
			CheckRowAddrDep(erase, ROW_ADDR_DEPENDENCY_CHECK_OPT_RELEASE));

	dep_entry(TEST_BLOCK)->blockedReadReqCnt = 0;
	TEST_ASSERT_EQUAL_UINT(ROW_ADDR_DEPENDENCY_REPORT_PASS,
			CheckRowAddrDep(erase, ROW_ADDR_DEPENDENCY_CHECK_OPT_RELEASE));
	TEST_ASSERT_EQUAL_UINT(0, dep_entry(TEST_BLOCK)->permittedProgPage);
	TEST_ASSERT_EQUAL_UINT(0, dep_entry(TEST_BLOCK)->blockedEraseReqFlag);
}

static void test_row_dep_rejects_unsupported_options(void)
{
	unsigned int read = make_nand_req(REQ_CODE_READ, TEST_BLOCK, 0);
	unsigned int erase = make_nand_req(REQ_CODE_ERASE, TEST_BLOCK, 0);
	unsigned int bogus = make_nand_req(REQ_CODE_RxDMA, TEST_BLOCK, 0);
	unsigned int phys = make_nand_req(REQ_CODE_READ, TEST_BLOCK, 0);

	req(erase)->nandInfo.programmedPageCnt = 5;
	req(phys)->reqOpt.nandAddr = REQ_OPT_NAND_ADDR_PHY_ORG;

	FW_EXPECT_ASSERT(CheckRowAddrDep(read, 0xFF));
	FW_EXPECT_ASSERT(CheckRowAddrDep(erase, 0xFF));
	FW_EXPECT_ASSERT(CheckRowAddrDep(bogus, ROW_ADDR_DEPENDENCY_CHECK_OPT_SELECT));
	FW_EXPECT_ASSERT(CheckRowAddrDep(phys, ROW_ADDR_DEPENDENCY_CHECK_OPT_SELECT));
}

/* ---- SelectLowLevelReqQ / ReleaseBlockedByRowAddrDepReq ---- */

static void test_blocked_read_is_released_once_page_is_written(void)
{
	unsigned int chNo = Vdie2PchTranslation(TEST_DIE);
	unsigned int wayNo = Vdie2PwayTranslation(TEST_DIE);
	unsigned int read = make_nand_req(REQ_CODE_READ, TEST_BLOCK, 0);
	unsigned int write = make_nand_req(REQ_CODE_WRITE, TEST_BLOCK, 0);

	SelectLowLevelReqQ(read);
	TEST_ASSERT_EQUAL_UINT(1, blockedByRowAddrDepReqQ[chNo][wayNo].reqCnt);
	TEST_ASSERT_EQUAL_UINT(1, blockedReqCnt);
	TEST_ASSERT_EQUAL_UINT(0, nandReqQ[chNo][wayNo].reqCnt);

	SelectLowLevelReqQ(write);
	TEST_ASSERT_EQUAL_UINT(1, nandReqQ[chNo][wayNo].reqCnt);

	ReleaseBlockedByRowAddrDepReq(chNo, wayNo);
	TEST_ASSERT_EQUAL_UINT(0, blockedByRowAddrDepReqQ[chNo][wayNo].reqCnt);
	TEST_ASSERT_EQUAL_UINT(0, blockedReqCnt);
	TEST_ASSERT_EQUAL_UINT(2, nandReqQ[chNo][wayNo].reqCnt);
}

static void test_blocked_erase_is_released_synchronously_by_later_read(void)
{
	unsigned int chNo = Vdie2PchTranslation(TEST_DIE);
	unsigned int wayNo = Vdie2PwayTranslation(TEST_DIE);
	unsigned int erase = make_nand_req(REQ_CODE_ERASE, TEST_BLOCK, 0);
	unsigned int read = make_nand_req(REQ_CODE_READ, TEST_BLOCK, 0);

	dep_entry(TEST_BLOCK)->permittedProgPage = 1;
	req(erase)->nandInfo.programmedPageCnt = 3;

	SelectLowLevelReqQ(erase);
	TEST_ASSERT_EQUAL_UINT(1, dep_entry(TEST_BLOCK)->blockedEraseReqFlag);
	TEST_ASSERT_EQUAL_UINT(1, blockedByRowAddrDepReqQ[chNo][wayNo].reqCnt);

	/* Outstanding programs finish, making the erase eligible. */
	dep_entry(TEST_BLOCK)->permittedProgPage = 3;
	SelectLowLevelReqQ(read);
	/* SyncReleaseEraseReq forced the erase through; the read now waits on the erased block. */
	TEST_ASSERT_EQUAL_UINT(0, dep_entry(TEST_BLOCK)->blockedEraseReqFlag);
	TEST_ASSERT_EQUAL_UINT(0, dep_entry(TEST_BLOCK)->permittedProgPage);
	TEST_ASSERT_EQUAL_UINT(1, blockedByRowAddrDepReqQ[chNo][wayNo].reqCnt);
	TEST_ASSERT_EQUAL_UINT(read, blockedByRowAddrDepReqQ[chNo][wayNo].headReq);
	TEST_ASSERT_EQUAL_UINT(1, dep_entry(TEST_BLOCK)->blockedReadReqCnt);

	dep_entry(TEST_BLOCK)->permittedProgPage = 1;
	ReleaseBlockedByRowAddrDepReq(chNo, wayNo);
	TEST_ASSERT_EQUAL_UINT(0, blockedByRowAddrDepReqQ[chNo][wayNo].reqCnt);

	SyncAllLowLevelReqDone();
	TEST_ASSERT_EQUAL_UINT(1, mock_nsc_count_cmd(V2FCommand_BlockErase));
	TEST_ASSERT_EQUAL_UINT(1, mock_nsc_count_cmd(V2FCommand_ReadPageTrigger));
}

static void test_select_without_row_dep_check_goes_straight_to_nand(void)
{
	unsigned int chNo = Vdie2PchTranslation(TEST_DIE);
	unsigned int wayNo = Vdie2PwayTranslation(TEST_DIE);
	unsigned int read = make_nand_req(REQ_CODE_READ, TEST_BLOCK, 9);

	req(read)->reqOpt.rowAddrDependencyCheck = REQ_OPT_ROW_ADDR_DEPENDENCY_NONE;
	SelectLowLevelReqQ(read);

	TEST_ASSERT_EQUAL_UINT(1, nandReqQ[chNo][wayNo].reqCnt);
	TEST_ASSERT_EQUAL_UINT(0, blockedReqCnt);
}

static void test_select_rejects_unsupported_request_options(void)
{
	unsigned int badType = make_nand_req(REQ_CODE_READ, TEST_BLOCK, 0);
	unsigned int badAddr = make_nand_req(REQ_CODE_READ, TEST_BLOCK, 0);
	unsigned int badDmaCode = make_nand_req(REQ_CODE_READ, TEST_BLOCK, 0);

	req(badType)->reqType = REQ_TYPE_SLICE;
	req(badAddr)->reqOpt.nandAddr = 0xFF;
	req(badDmaCode)->reqType = REQ_TYPE_NVME_DMA;
	req(badDmaCode)->reqOpt.dataBufFormat = REQ_OPT_DATA_BUF_ENTRY;
	req(badDmaCode)->dataBufInfo.entry = 0;

	FW_EXPECT_ASSERT(SelectLowLevelReqQ(badType));
	FW_EXPECT_ASSERT(SelectLowLevelReqQ(badAddr));
	FW_EXPECT_ASSERT(SelectLowLevelReqQ(badDmaCode));
}

static void test_buffer_blocked_nand_read_updates_row_dep_table(void)
{
	unsigned int first = make_nand_req(REQ_CODE_READ, TEST_BLOCK, 0);
	unsigned int second = make_nand_req(REQ_CODE_READ, TEST_BLOCK, 0);
	unsigned int erase = make_nand_req(REQ_CODE_ERASE, TEST_BLOCK, 0);

	req(second)->prevBlockingReq = first;
	req(first)->nextBlockingReq = second;
	req(erase)->prevBlockingReq = first;

	SelectLowLevelReqQ(second);
	TEST_ASSERT_EQUAL_UINT(1, blockedByBufDepReqQ.reqCnt);
	TEST_ASSERT_EQUAL_UINT(1, dep_entry(TEST_BLOCK)->blockedReadReqCnt);

	SelectLowLevelReqQ(erase);
	TEST_ASSERT_EQUAL_UINT(2, blockedByBufDepReqQ.reqCnt);
	TEST_ASSERT_EQUAL_UINT(1, dep_entry(TEST_BLOCK)->blockedEraseReqFlag);
}

static void test_buffer_blocked_read_behind_blocked_erase_is_synced(void)
{
	unsigned int chNo = Vdie2PchTranslation(TEST_DIE);
	unsigned int wayNo = Vdie2PwayTranslation(TEST_DIE);
	unsigned int erase = make_nand_req(REQ_CODE_ERASE, TEST_BLOCK, 0);
	unsigned int read = make_nand_req(REQ_CODE_READ, TEST_BLOCK, 0);
	unsigned int iterations = 0;

	req(erase)->nandInfo.programmedPageCnt = 4;
	SelectLowLevelReqQ(erase);
	TEST_ASSERT_EQUAL_UINT(1, dep_entry(TEST_BLOCK)->blockedEraseReqFlag);

	/* The read is buffer-blocked behind the erase; programs then finish. */
	req(read)->prevBlockingReq = erase;
	req(erase)->nextBlockingReq = read;
	dep_entry(TEST_BLOCK)->permittedProgPage = 4;
	SelectLowLevelReqQ(read);

	TEST_ASSERT_EQUAL_UINT(0, dep_entry(TEST_BLOCK)->blockedEraseReqFlag);
	TEST_ASSERT_EQUAL_UINT(1, dep_entry(TEST_BLOCK)->blockedReadReqCnt);
	TEST_ASSERT_EQUAL_UINT(1, blockedReqCnt);
	TEST_ASSERT_EQUAL_UINT(1, blockedByBufDepReqQ.reqCnt + blockedByRowAddrDepReqQ[chNo][wayNo].reqCnt);

	dep_entry(TEST_BLOCK)->permittedProgPage = 1;
	while ((notCompletedNandReqCnt || blockedReqCnt) && iterations++ < 1000)
		SchedulingNandReq();

	TEST_ASSERT_EQUAL_UINT(0, blockedReqCnt);
	TEST_ASSERT_EQUAL_UINT(0, dep_entry(TEST_BLOCK)->blockedReadReqCnt);
	TEST_ASSERT_EQUAL_UINT(1, mock_nsc_count_cmd(V2FCommand_BlockErase));
	TEST_ASSERT_EQUAL_UINT(1, mock_nsc_count_cmd(V2FCommand_ReadPageTrigger));
}

/* ---- IssueNvmeDmaReq / CheckDoneNvmeDmaReq ---- */

static unsigned int make_dma_req(unsigned int reqCode, unsigned int numOfNvmeBlock)
{
	unsigned int reqSlotTag = GetFromFreeReqQ();

	req(reqSlotTag)->reqType = REQ_TYPE_NVME_DMA;
	req(reqSlotTag)->reqCode = reqCode;
	req(reqSlotTag)->nvmeCmdSlotTag = CMD_SLOT;
	req(reqSlotTag)->reqOpt.dataBufFormat = REQ_OPT_DATA_BUF_ENTRY;
	req(reqSlotTag)->dataBufInfo.entry = 1;
	req(reqSlotTag)->nvmeDmaInfo.startIndex = 2;
	req(reqSlotTag)->nvmeDmaInfo.nvmeBlockOffset = 0;
	req(reqSlotTag)->nvmeDmaInfo.numOfNvmeBlock = numOfNvmeBlock;
	req(reqSlotTag)->prevBlockingReq = REQ_SLOT_TAG_NONE;
	req(reqSlotTag)->nextBlockingReq = REQ_SLOT_TAG_NONE;
	return reqSlotTag;
}

static void test_issue_rx_dma_programs_one_transfer_per_block(void)
{
	unsigned int reqSlotTag = make_dma_req(REQ_CODE_RxDMA, 3);
	const mock_host_call_t *first;

	IssueNvmeDmaReq(reqSlotTag);

	TEST_ASSERT_EQUAL_UINT(3, mock_host_count(MOCK_HOST_SET_AUTO_RX_DMA));
	first = mock_host_call_at(0);
	TEST_ASSERT_EQUAL_UINT(MOCK_HOST_SET_AUTO_RX_DMA, first->kind);
	TEST_ASSERT_EQUAL_UINT(CMD_SLOT, first->args[0]);
	TEST_ASSERT_EQUAL_UINT(2, first->args[1]);
	TEST_ASSERT_EQUAL_UINT(first->args[2] + 2 * BYTES_PER_NVME_BLOCK,
			mock_host_last(MOCK_HOST_SET_AUTO_RX_DMA)->args[2]);
	TEST_ASSERT_EQUAL_UINT(g_hostDmaStatus.fifoTail.autoDmaRx, req(reqSlotTag)->nvmeDmaInfo.reqTail);
}

static void test_issue_dma_rejects_non_dma_request_code(void)
{
	unsigned int reqSlotTag = make_dma_req(REQ_CODE_READ, 1);

	FW_EXPECT_ASSERT(IssueNvmeDmaReq(reqSlotTag));
}

static void test_check_done_retires_only_completed_dma_requests(void)
{
	unsigned int rx = make_dma_req(REQ_CODE_RxDMA, 1);
	unsigned int tx = make_dma_req(REQ_CODE_TxDMA, 1);

	SelectLowLevelReqQ(rx);
	SelectLowLevelReqQ(tx);
	TEST_ASSERT_EQUAL_UINT(2, nvmeDmaReqQ.reqCnt);

	mock_host_set_partial_done(0);
	CheckDoneNvmeDmaReq();
	TEST_ASSERT_EQUAL_UINT(2, nvmeDmaReqQ.reqCnt);

	mock_host_set_partial_done(1);
	CheckDoneNvmeDmaReq();
	TEST_ASSERT_EQUAL_UINT(0, nvmeDmaReqQ.reqCnt);
	TEST_ASSERT_EQUAL_UINT(REQ_QUEUE_TYPE_FREE, req(rx)->reqQueueType);
	TEST_ASSERT_EQUAL_UINT(REQ_QUEUE_TYPE_FREE, req(tx)->reqQueueType);
}

static void test_releasing_dma_unblocks_chained_dma_on_same_buffer(void)
{
	unsigned int first = make_dma_req(REQ_CODE_TxDMA, 1);
	unsigned int second = make_dma_req(REQ_CODE_TxDMA, 1);

	UpdateDataBufEntryInfoBlockingReq(1, first);
	UpdateDataBufEntryInfoBlockingReq(1, second);
	SelectLowLevelReqQ(first);
	SelectLowLevelReqQ(second);
	TEST_ASSERT_EQUAL_UINT(1, nvmeDmaReqQ.reqCnt);
	TEST_ASSERT_EQUAL_UINT(1, blockedByBufDepReqQ.reqCnt);

	CheckDoneNvmeDmaReq();
	TEST_ASSERT_EQUAL_UINT(0, blockedByBufDepReqQ.reqCnt);
	TEST_ASSERT_EQUAL_UINT(2, mock_host_count(MOCK_HOST_SET_AUTO_TX_DMA));
	TEST_ASSERT_EQUAL_UINT(second, nvmeDmaReqQ.headReq);
	TEST_ASSERT_EQUAL_UINT(second, dataBufMapPtr->dataBuf[1].blockingReqTail);

	CheckDoneNvmeDmaReq();
	TEST_ASSERT_EQUAL_UINT(0, nvmeDmaReqQ.reqCnt);
	TEST_ASSERT_EQUAL_UINT(REQ_SLOT_TAG_NONE, dataBufMapPtr->dataBuf[1].blockingReqTail);
}

int main(void)
{
	UNITY_BEGIN();
	RUN_TEST(test_smoke_single_slice_read_becomes_one_slice_request);
	RUN_TEST(test_smoke_two_slice_write_is_split);
	RUN_TEST(test_unaligned_range_spanning_three_slices_sets_dma_windows);
	RUN_TEST(test_range_ending_on_slice_boundary_has_no_tail_request);
	RUN_TEST(test_unsupported_command_code_asserts);
	RUN_TEST(test_read_of_unwritten_slice_skips_nand_and_issues_tx_dma);
	RUN_TEST(test_full_slice_write_issues_rx_dma_and_marks_buffer_dirty);
	RUN_TEST(test_partial_write_of_written_slice_reads_nand_first);
	RUN_TEST(test_read_of_written_slice_reads_nand_then_tx_dma);
	RUN_TEST(test_second_access_to_same_slice_hits_data_buffer);
	RUN_TEST(test_evicting_dirty_buffer_writes_it_back_to_nand);
	RUN_TEST(test_row_dep_write_passes_only_at_permitted_page);
	RUN_TEST(test_row_dep_read_blocks_until_page_programmed);
	RUN_TEST(test_row_dep_erase_waits_for_programmed_pages_and_blocked_reads);
	RUN_TEST(test_row_dep_rejects_unsupported_options);
	RUN_TEST(test_blocked_read_is_released_once_page_is_written);
	RUN_TEST(test_blocked_erase_is_released_synchronously_by_later_read);
	RUN_TEST(test_select_without_row_dep_check_goes_straight_to_nand);
	RUN_TEST(test_select_rejects_unsupported_request_options);
	RUN_TEST(test_buffer_blocked_nand_read_updates_row_dep_table);
	RUN_TEST(test_buffer_blocked_read_behind_blocked_erase_is_synced);
	RUN_TEST(test_issue_rx_dma_programs_one_transfer_per_block);
	RUN_TEST(test_issue_dma_rejects_non_dma_request_code);
	RUN_TEST(test_check_done_retires_only_completed_dma_requests);
	RUN_TEST(test_releasing_dma_unblocks_chained_dma_on_same_buffer);
	return UNITY_END();
}
