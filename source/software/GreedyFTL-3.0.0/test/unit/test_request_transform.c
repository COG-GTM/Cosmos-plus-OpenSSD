/* Unit tests for request_transform.c. */
#include "unity.h"

#include "fw_test.h"
#include "memory_map.h"

/* Internal helpers of request_transform.c (not exported through its header). */
void EvictDataBufEntry(unsigned int originReqSlotTag);
void DataReadFromNand(unsigned int originReqSlotTag);
unsigned int CheckBufDep(unsigned int reqSlotTag);
unsigned int CheckRowAddrDep(unsigned int reqSlotTag, unsigned int checkRowAddrDepOpt);
unsigned int UpdateRowAddrDepTableForBufBlockedReq(unsigned int reqSlotTag);

#define FULL_SLICE_NLB (NVME_BLOCKS_PER_SLICE - 1)

void setUp(void)
{
	fw_test_reset();
	fw_test_init_ftl();
	mock_nsc_reset();
	mock_host_reset();
}

void tearDown(void) {}

/* ------------------------------------------------------------------------ */
/* Helpers                                                                  */
/* ------------------------------------------------------------------------ */

static P_SSD_REQ_FORMAT req(unsigned int tag) { return &reqPoolPtr->reqPool[tag]; }

static P_ROW_ADDR_DEPENDENCY_ENTRY dep_entry_for_vsa(unsigned int vsa)
{
	unsigned int die = Vsa2VdieTranslation(vsa);
	return &rowAddrDependencyTablePtr->block[Vdie2PchTranslation(die)][Vdie2PwayTranslation(die)]
		[Vsa2VblockTranslation(vsa)];
}

static unsigned int ch_of_vsa(unsigned int vsa) { return Vdie2PchTranslation(Vsa2VdieTranslation(vsa)); }
static unsigned int way_of_vsa(unsigned int vsa) { return Vdie2PwayTranslation(Vsa2VdieTranslation(vsa)); }

/* Allocates a NAND request addressed by VSA with row-address dependency checking on. */
static unsigned int new_nand_req(unsigned int reqCode, unsigned int vsa)
{
	unsigned int tag = GetFromFreeReqQ();

	req(tag)->reqType = REQ_TYPE_NAND;
	req(tag)->reqCode = reqCode;
	req(tag)->nvmeCmdSlotTag = 0;
	req(tag)->logicalSliceAddr = 0;
	req(tag)->reqOpt.dataBufFormat = REQ_OPT_DATA_BUF_NONE;
	req(tag)->reqOpt.nandAddr = REQ_OPT_NAND_ADDR_VSA;
	req(tag)->reqOpt.nandEcc = REQ_OPT_NAND_ECC_ON;
	req(tag)->reqOpt.nandEccWarning = REQ_OPT_NAND_ECC_WARNING_ON;
	req(tag)->reqOpt.rowAddrDependencyCheck = REQ_OPT_ROW_ADDR_DEPENDENCY_CHECK;
	req(tag)->reqOpt.blockSpace = REQ_OPT_BLOCK_SPACE_MAIN;
	req(tag)->nandInfo.virtualSliceAddr = vsa;
	req(tag)->prevBlockingReq = REQ_SLOT_TAG_NONE;
	req(tag)->nextBlockingReq = REQ_SLOT_TAG_NONE;
	return tag;
}

/* Allocates a slice request the way ReqTransNvmeToSlice would. */
static unsigned int new_slice_req(unsigned int reqCode, unsigned int lsa, unsigned int numOfNvmeBlock)
{
	unsigned int tag = GetFromFreeReqQ();

	req(tag)->reqType = REQ_TYPE_SLICE;
	req(tag)->reqCode = reqCode;
	req(tag)->nvmeCmdSlotTag = 3;
	req(tag)->logicalSliceAddr = lsa;
	req(tag)->nvmeDmaInfo.startIndex = 0;
	req(tag)->nvmeDmaInfo.nvmeBlockOffset = 0;
	req(tag)->nvmeDmaInfo.numOfNvmeBlock = numOfNvmeBlock;
	req(tag)->prevBlockingReq = REQ_SLOT_TAG_NONE;
	req(tag)->nextBlockingReq = REQ_SLOT_TAG_NONE;
	return tag;
}

static unsigned int slice_req_at(unsigned int index)
{
	unsigned int tag = sliceReqQ.headReq;

	while (index--)
		tag = req(tag)->nextReq;
	return tag;
}

static unsigned int total_nand_req_count(void)
{
	unsigned int ch, way, total = 0;

	for (ch = 0; ch < USER_CHANNELS; ch++)
		for (way = 0; way < USER_WAYS; way++)
			total += nandReqQ[ch][way].reqCnt;
	return total;
}

static void assert_slice_req(unsigned int tag, unsigned int reqCode, unsigned int lsa, unsigned int startIndex,
		unsigned int blockOffset, unsigned int numOfNvmeBlock)
{
	TEST_ASSERT_EQUAL_UINT(REQ_TYPE_SLICE, req(tag)->reqType);
	TEST_ASSERT_EQUAL_UINT(reqCode, req(tag)->reqCode);
	TEST_ASSERT_EQUAL_UINT(lsa, req(tag)->logicalSliceAddr);
	TEST_ASSERT_EQUAL_UINT(startIndex, req(tag)->nvmeDmaInfo.startIndex);
	TEST_ASSERT_EQUAL_UINT(blockOffset, req(tag)->nvmeDmaInfo.nvmeBlockOffset);
	TEST_ASSERT_EQUAL_UINT(numOfNvmeBlock, req(tag)->nvmeDmaInfo.numOfNvmeBlock);
}

/* ------------------------------------------------------------------------ */
/* InitDependencyTable                                                      */
/* ------------------------------------------------------------------------ */

static void test_init_dependency_table_clears_every_entry(void)
{
	P_ROW_ADDR_DEPENDENCY_ENTRY e;
	unsigned int ch, way, blk;

	for (ch = 0; ch < USER_CHANNELS; ch++)
		for (way = 0; way < USER_WAYS; way++)
			for (blk = 0; blk < MAIN_BLOCKS_PER_DIE; blk++) {
				e = &rowAddrDependencyTablePtr->block[ch][way][blk];
				e->permittedProgPage = 7;
				e->blockedReadReqCnt = 3;
				e->blockedEraseReqFlag = 1;
			}

	InitDependencyTable();

	TEST_ASSERT_EQUAL_PTR(fw_ptr(ROW_ADDR_DEPENDENCY_TABLE_ADDR), rowAddrDependencyTablePtr);
	for (ch = 0; ch < USER_CHANNELS; ch++)
		for (way = 0; way < USER_WAYS; way++)
			for (blk = 0; blk < MAIN_BLOCKS_PER_DIE; blk++) {
				e = &rowAddrDependencyTablePtr->block[ch][way][blk];
				TEST_ASSERT_EQUAL_UINT(0, e->permittedProgPage);
				TEST_ASSERT_EQUAL_UINT(0, e->blockedReadReqCnt);
				TEST_ASSERT_EQUAL_UINT(0, e->blockedEraseReqFlag);
			}
}

/* ------------------------------------------------------------------------ */
/* ReqTransNvmeToSlice                                                      */
/* ------------------------------------------------------------------------ */

static void test_nvme_to_slice_single_block_read(void)
{
	/* nlb is zero based: 0 means one 4 KiB NVMe block. */
	ReqTransNvmeToSlice(7, 0, 0, IO_NVM_READ);

	TEST_ASSERT_EQUAL_UINT(1, sliceReqQ.reqCnt);
	assert_slice_req(sliceReqQ.headReq, REQ_CODE_READ, 0, 0, 0, 1);
	TEST_ASSERT_EQUAL_UINT(7, req(sliceReqQ.headReq)->nvmeCmdSlotTag);
}

static void test_nvme_to_slice_aligned_two_full_slices(void)
{
	ReqTransNvmeToSlice(0, 0, 2 * NVME_BLOCKS_PER_SLICE - 1, IO_NVM_WRITE);

	TEST_ASSERT_EQUAL_UINT(2, sliceReqQ.reqCnt);
	assert_slice_req(slice_req_at(0), REQ_CODE_WRITE, 0, 0, 0, NVME_BLOCKS_PER_SLICE);
	assert_slice_req(slice_req_at(1), REQ_CODE_WRITE, 1, NVME_BLOCKS_PER_SLICE, 0, NVME_BLOCKS_PER_SLICE);
}

static void test_nvme_to_slice_unaligned_start_within_one_slice(void)
{
	/* LBA 1, 2 blocks: stays inside slice 0 (loop == 0). */
	ReqTransNvmeToSlice(0, 1, 1, IO_NVM_READ);

	TEST_ASSERT_EQUAL_UINT(1, sliceReqQ.reqCnt);
	assert_slice_req(sliceReqQ.headReq, REQ_CODE_READ, 0, 0, 1, 2);
}

static void test_nvme_to_slice_unaligned_start_spanning_two_slices(void)
{
	/* LBA 1, NVME_BLOCKS_PER_SLICE blocks: tail of slice 0 + head of slice 1. */
	ReqTransNvmeToSlice(0, 1, FULL_SLICE_NLB, IO_NVM_READ);

	TEST_ASSERT_EQUAL_UINT(2, sliceReqQ.reqCnt);
	assert_slice_req(slice_req_at(0), REQ_CODE_READ, 0, 0, 1, NVME_BLOCKS_PER_SLICE - 1);
	assert_slice_req(slice_req_at(1), REQ_CODE_READ, 1, NVME_BLOCKS_PER_SLICE - 1, 0, 1);
}

static void test_nvme_to_slice_unaligned_start_with_middle_and_partial_tail(void)
{
	unsigned int startLba = 10 * NVME_BLOCKS_PER_SLICE + 2;
	unsigned int nlb = (NVME_BLOCKS_PER_SLICE - 2) + NVME_BLOCKS_PER_SLICE + 1 - 1;

	ReqTransNvmeToSlice(0, startLba, nlb, IO_NVM_WRITE);

	TEST_ASSERT_EQUAL_UINT(3, sliceReqQ.reqCnt);
	assert_slice_req(slice_req_at(0), REQ_CODE_WRITE, 10, 0, 2, NVME_BLOCKS_PER_SLICE - 2);
	assert_slice_req(slice_req_at(1), REQ_CODE_WRITE, 11, NVME_BLOCKS_PER_SLICE - 2, 0, NVME_BLOCKS_PER_SLICE);
	assert_slice_req(slice_req_at(2), REQ_CODE_WRITE, 12, 2 * NVME_BLOCKS_PER_SLICE - 2, 0, 1);
}

static void test_nvme_to_slice_aligned_start_with_partial_tail(void)
{
	ReqTransNvmeToSlice(0, 0, NVME_BLOCKS_PER_SLICE, IO_NVM_READ);

	TEST_ASSERT_EQUAL_UINT(2, sliceReqQ.reqCnt);
	assert_slice_req(slice_req_at(0), REQ_CODE_READ, 0, 0, 0, NVME_BLOCKS_PER_SLICE);
	assert_slice_req(slice_req_at(1), REQ_CODE_READ, 1, NVME_BLOCKS_PER_SLICE, 0, 1);
}

static void test_nvme_to_slice_unaligned_end_exactly_on_boundary_has_no_tail(void)
{
	/* LBA 2 .. end of slice 1: first (partial) + one full, tail == 0. */
	ReqTransNvmeToSlice(0, 2, (2 * NVME_BLOCKS_PER_SLICE - 2) - 1, IO_NVM_READ);

	TEST_ASSERT_EQUAL_UINT(2, sliceReqQ.reqCnt);
	assert_slice_req(slice_req_at(0), REQ_CODE_READ, 0, 0, 2, NVME_BLOCKS_PER_SLICE - 2);
	assert_slice_req(slice_req_at(1), REQ_CODE_READ, 1, NVME_BLOCKS_PER_SLICE - 2, 0, NVME_BLOCKS_PER_SLICE);
}

static void test_nvme_to_slice_rejects_unknown_command_code(void)
{
	FW_EXPECT_ASSERT(ReqTransNvmeToSlice(0, 0, 0, IO_NVM_FLUSH));
}

/* ------------------------------------------------------------------------ */
/* ReqTransSliceToLowLevel                                                  */
/* ------------------------------------------------------------------------ */

static void test_slice_to_low_level_read_miss_reads_nand_and_issues_tx_dma(void)
{
	unsigned int tag, vsa, entry;

	/* Give LSA 5 a valid, programmed mapping so the read hits NAND. */
	vsa = AddrTransWrite(5);
	dep_entry_for_vsa(vsa)->permittedProgPage = Vsa2VpageTranslation(vsa) + 1;
	ReqTransNvmeToSlice(2, 5 * NVME_BLOCKS_PER_SLICE, FULL_SLICE_NLB, IO_NVM_READ);
	tag = sliceReqQ.headReq;

	ReqTransSliceToLowLevel();

	TEST_ASSERT_EQUAL_UINT(0, sliceReqQ.reqCnt);
	TEST_ASSERT_EQUAL_UINT(REQ_TYPE_NVME_DMA, req(tag)->reqType);
	TEST_ASSERT_EQUAL_UINT(REQ_CODE_TxDMA, req(tag)->reqCode);
	TEST_ASSERT_EQUAL_UINT(REQ_OPT_DATA_BUF_ENTRY, req(tag)->reqOpt.dataBufFormat);
	entry = req(tag)->dataBufInfo.entry;
	TEST_ASSERT_EQUAL_UINT(5, dataBufMapPtr->dataBuf[entry].logicalSliceAddr);
	TEST_ASSERT_EQUAL_UINT(DATA_BUF_CLEAN, dataBufMapPtr->dataBuf[entry].dirty);

	/* One NAND read was queued and the DMA is blocked behind it. */
	TEST_ASSERT_EQUAL_UINT(1, nandReqQ[ch_of_vsa(vsa)][way_of_vsa(vsa)].reqCnt);
	TEST_ASSERT_EQUAL_UINT(REQ_QUEUE_TYPE_BLOCKED_BY_BUF_DEP, req(tag)->reqQueueType);
	TEST_ASSERT_EQUAL_UINT(1, blockedByBufDepReqQ.reqCnt);
	TEST_ASSERT_EQUAL_UINT(0, mock_host_count(MOCK_HOST_SET_AUTO_TX_DMA));
}

static void test_slice_to_low_level_read_of_unmapped_lsa_skips_nand(void)
{
	unsigned int tag;

	ReqTransNvmeToSlice(2, 0, FULL_SLICE_NLB, IO_NVM_READ);
	tag = sliceReqQ.headReq;

	ReqTransSliceToLowLevel();

	TEST_ASSERT_EQUAL_UINT(0, total_nand_req_count());
	TEST_ASSERT_EQUAL_UINT(REQ_CODE_TxDMA, req(tag)->reqCode);
	TEST_ASSERT_EQUAL_UINT(REQ_QUEUE_TYPE_NVME_DMA, req(tag)->reqQueueType);
	TEST_ASSERT_EQUAL_UINT(NVME_BLOCKS_PER_SLICE, mock_host_count(MOCK_HOST_SET_AUTO_TX_DMA));
}

static void test_slice_to_low_level_full_slice_write_marks_dirty_and_issues_rx_dma(void)
{
	unsigned int tag, entry;
	const mock_host_call_t *dma;

	ReqTransNvmeToSlice(4, 0, FULL_SLICE_NLB, IO_NVM_WRITE);
	tag = sliceReqQ.headReq;

	ReqTransSliceToLowLevel();

	entry = req(tag)->dataBufInfo.entry;
	TEST_ASSERT_EQUAL_UINT(REQ_TYPE_NVME_DMA, req(tag)->reqType);
	TEST_ASSERT_EQUAL_UINT(REQ_CODE_RxDMA, req(tag)->reqCode);
	TEST_ASSERT_EQUAL_UINT(DATA_BUF_DIRTY, dataBufMapPtr->dataBuf[entry].dirty);
	TEST_ASSERT_EQUAL_UINT(0, dataBufMapPtr->dataBuf[entry].logicalSliceAddr);
	TEST_ASSERT_EQUAL_UINT(tag, dataBufMapPtr->dataBuf[entry].blockingReqTail);

	/* No read-modify-write for a full slice. */
	TEST_ASSERT_EQUAL_UINT(0, total_nand_req_count());
	TEST_ASSERT_EQUAL_UINT(1, nvmeDmaReqQ.reqCnt);
	TEST_ASSERT_EQUAL_UINT(NVME_BLOCKS_PER_SLICE, mock_host_count(MOCK_HOST_SET_AUTO_RX_DMA));
	dma = mock_host_last(MOCK_HOST_SET_AUTO_RX_DMA);
	TEST_ASSERT_NOT_NULL(dma);
	TEST_ASSERT_EQUAL_UINT(4, dma->args[0]);
	TEST_ASSERT_EQUAL_UINT(NVME_BLOCKS_PER_SLICE - 1, dma->args[1]);
	TEST_ASSERT_EQUAL_UINT(NVME_COMMAND_AUTO_COMPLETION_ON, dma->args[3]);
}

static void test_slice_to_low_level_partial_write_of_mapped_slice_triggers_read_modify_write(void)
{
	unsigned int tag, vsa;

	vsa = AddrTransWrite(3);
	dep_entry_for_vsa(vsa)->permittedProgPage = Vsa2VpageTranslation(vsa) + 1;
	ReqTransNvmeToSlice(0, 3 * NVME_BLOCKS_PER_SLICE + 1, 0, IO_NVM_WRITE);
	tag = sliceReqQ.headReq;

	ReqTransSliceToLowLevel();

	TEST_ASSERT_EQUAL_UINT(1, nandReqQ[ch_of_vsa(vsa)][way_of_vsa(vsa)].reqCnt);
	TEST_ASSERT_EQUAL_UINT(REQ_CODE_READ, req(nandReqQ[ch_of_vsa(vsa)][way_of_vsa(vsa)].headReq)->reqCode);
	TEST_ASSERT_EQUAL_UINT(REQ_CODE_RxDMA, req(tag)->reqCode);
	TEST_ASSERT_EQUAL_UINT(REQ_QUEUE_TYPE_BLOCKED_BY_BUF_DEP, req(tag)->reqQueueType);
}

static void test_slice_to_low_level_partial_write_of_unmapped_slice_has_no_nand_read(void)
{
	ReqTransNvmeToSlice(0, 1, 0, IO_NVM_WRITE);

	ReqTransSliceToLowLevel();

	TEST_ASSERT_EQUAL_UINT(0, total_nand_req_count());
	TEST_ASSERT_EQUAL_UINT(1, nvmeDmaReqQ.reqCnt);
	TEST_ASSERT_EQUAL_UINT(1, mock_host_count(MOCK_HOST_SET_AUTO_RX_DMA));
}

static void test_slice_to_low_level_hit_reuses_buffer_and_chains_behind_previous_request(void)
{
	unsigned int first, second;

	ReqTransNvmeToSlice(0, 0, FULL_SLICE_NLB, IO_NVM_WRITE);
	first = sliceReqQ.headReq;
	ReqTransSliceToLowLevel();

	ReqTransNvmeToSlice(1, 0, FULL_SLICE_NLB, IO_NVM_READ);
	second = sliceReqQ.headReq;
	ReqTransSliceToLowLevel();

	TEST_ASSERT_EQUAL_UINT(req(first)->dataBufInfo.entry, req(second)->dataBufInfo.entry);
	TEST_ASSERT_EQUAL_UINT(first, req(second)->prevBlockingReq);
	TEST_ASSERT_EQUAL_UINT(second, req(first)->nextBlockingReq);
	TEST_ASSERT_EQUAL_UINT(REQ_CODE_TxDMA, req(second)->reqCode);
	TEST_ASSERT_EQUAL_UINT(REQ_QUEUE_TYPE_BLOCKED_BY_BUF_DEP, req(second)->reqQueueType);
	/* A hit never touches NAND. */
	TEST_ASSERT_EQUAL_UINT(0, total_nand_req_count());
}

static void test_slice_to_low_level_processes_every_queued_slice(void)
{
	ReqTransNvmeToSlice(0, 0, 3 * NVME_BLOCKS_PER_SLICE - 1, IO_NVM_WRITE);
	TEST_ASSERT_EQUAL_UINT(3, sliceReqQ.reqCnt);

	ReqTransSliceToLowLevel();

	TEST_ASSERT_EQUAL_UINT(0, sliceReqQ.reqCnt);
	TEST_ASSERT_EQUAL_UINT(REQ_SLOT_TAG_NONE, sliceReqQ.headReq);
	TEST_ASSERT_EQUAL_UINT(3, nvmeDmaReqQ.reqCnt);
	TEST_ASSERT_EQUAL_UINT(3 * NVME_BLOCKS_PER_SLICE, mock_host_count(MOCK_HOST_SET_AUTO_RX_DMA));
}

static void test_slice_to_low_level_with_empty_queue_is_noop(void)
{
	ReqTransSliceToLowLevel();

	TEST_ASSERT_EQUAL_UINT(0, nvmeDmaReqQ.reqCnt);
	TEST_ASSERT_EQUAL_UINT(0, mock_host_call_count());
}

static void test_slice_to_low_level_rejects_unknown_req_code(void)
{
	unsigned int tag = new_slice_req(REQ_CODE_ERASE, 0, NVME_BLOCKS_PER_SLICE);

	PutToSliceReqQ(tag);

	FW_EXPECT_ASSERT(ReqTransSliceToLowLevel());
}

/* ------------------------------------------------------------------------ */
/* EvictDataBufEntry (write-back to NAND)                                   */
/* ------------------------------------------------------------------------ */

static void test_evict_clean_entry_does_nothing(void)
{
	unsigned int tag = new_slice_req(REQ_CODE_WRITE, 0, NVME_BLOCKS_PER_SLICE);

	req(tag)->dataBufInfo.entry = 0;
	dataBufMapPtr->dataBuf[0].dirty = DATA_BUF_CLEAN;

	EvictDataBufEntry(tag);

	TEST_ASSERT_EQUAL_UINT(0, total_nand_req_count());
	TEST_ASSERT_EQUAL_UINT(0, blockedByBufDepReqQ.reqCnt);
}

static void test_evict_dirty_entry_queues_nand_write_and_cleans_entry(void)
{
	unsigned int tag, writeTag, vsa;

	tag = new_slice_req(REQ_CODE_WRITE, 42, NVME_BLOCKS_PER_SLICE);
	req(tag)->nvmeCmdSlotTag = 9;
	req(tag)->dataBufInfo.entry = 1;
	dataBufMapPtr->dataBuf[1].dirty = DATA_BUF_DIRTY;
	dataBufMapPtr->dataBuf[1].logicalSliceAddr = 17;
	dataBufMapPtr->dataBuf[1].blockingReqTail = REQ_SLOT_TAG_NONE;

	EvictDataBufEntry(tag);

	vsa = AddrTransRead(17);
	TEST_ASSERT_NOT_EQUAL(VSA_FAIL, vsa);
	TEST_ASSERT_EQUAL_UINT(DATA_BUF_CLEAN, dataBufMapPtr->dataBuf[1].dirty);
	TEST_ASSERT_EQUAL_UINT(1, total_nand_req_count());

	writeTag = nandReqQ[ch_of_vsa(vsa)][way_of_vsa(vsa)].headReq;
	TEST_ASSERT_EQUAL_UINT(REQ_TYPE_NAND, req(writeTag)->reqType);
	TEST_ASSERT_EQUAL_UINT(REQ_CODE_WRITE, req(writeTag)->reqCode);
	TEST_ASSERT_EQUAL_UINT(9, req(writeTag)->nvmeCmdSlotTag);
	TEST_ASSERT_EQUAL_UINT(17, req(writeTag)->logicalSliceAddr);
	TEST_ASSERT_EQUAL_UINT(vsa, req(writeTag)->nandInfo.virtualSliceAddr);
	TEST_ASSERT_EQUAL_UINT(1, req(writeTag)->dataBufInfo.entry);
	TEST_ASSERT_EQUAL_UINT(REQ_OPT_DATA_BUF_ENTRY, req(writeTag)->reqOpt.dataBufFormat);
	TEST_ASSERT_EQUAL_UINT(REQ_OPT_NAND_ADDR_VSA, req(writeTag)->reqOpt.nandAddr);
	TEST_ASSERT_EQUAL_UINT(REQ_OPT_NAND_ECC_ON, req(writeTag)->reqOpt.nandEcc);
	TEST_ASSERT_EQUAL_UINT(REQ_OPT_ROW_ADDR_DEPENDENCY_CHECK, req(writeTag)->reqOpt.rowAddrDependencyCheck);
	TEST_ASSERT_EQUAL_UINT(writeTag, dataBufMapPtr->dataBuf[1].blockingReqTail);
	/* The write claimed the page: the next program on this block is page + 1. */
	TEST_ASSERT_EQUAL_UINT(Vsa2VpageTranslation(vsa) + 1, dep_entry_for_vsa(vsa)->permittedProgPage);
}

static void test_evict_dirty_entry_blocked_behind_pending_rx_dma(void)
{
	unsigned int dmaTag, tag, writeTag, entry;

	/* Full-slice write leaves a dirty entry whose blocking tail is the RxDMA. */
	ReqTransNvmeToSlice(0, 0, FULL_SLICE_NLB, IO_NVM_WRITE);
	dmaTag = sliceReqQ.headReq;
	ReqTransSliceToLowLevel();
	entry = req(dmaTag)->dataBufInfo.entry;

	tag = new_slice_req(REQ_CODE_WRITE, 99, NVME_BLOCKS_PER_SLICE);
	req(tag)->dataBufInfo.entry = entry;

	EvictDataBufEntry(tag);

	TEST_ASSERT_EQUAL_UINT(DATA_BUF_CLEAN, dataBufMapPtr->dataBuf[entry].dirty);
	TEST_ASSERT_EQUAL_UINT(0, total_nand_req_count());
	TEST_ASSERT_EQUAL_UINT(1, blockedByBufDepReqQ.reqCnt);
	writeTag = blockedByBufDepReqQ.headReq;
	TEST_ASSERT_EQUAL_UINT(REQ_CODE_WRITE, req(writeTag)->reqCode);
	TEST_ASSERT_EQUAL_UINT(dmaTag, req(writeTag)->prevBlockingReq);
	TEST_ASSERT_EQUAL_UINT(writeTag, req(dmaTag)->nextBlockingReq);
}

static void test_write_back_runs_end_to_end_through_nand_scheduler(void)
{
	unsigned int dmaTag, tag, entry;

	ReqTransNvmeToSlice(0, 0, FULL_SLICE_NLB, IO_NVM_WRITE);
	dmaTag = sliceReqQ.headReq;
	ReqTransSliceToLowLevel();
	entry = req(dmaTag)->dataBufInfo.entry;

	/* Host DMA finishes, releasing the buffer dependency chain. */
	CheckDoneNvmeDmaReq();
	TEST_ASSERT_EQUAL_UINT(0, nvmeDmaReqQ.reqCnt);

	tag = new_slice_req(REQ_CODE_WRITE, 99, NVME_BLOCKS_PER_SLICE);
	req(tag)->dataBufInfo.entry = entry;
	EvictDataBufEntry(tag);

	TEST_ASSERT_EQUAL_UINT(1, total_nand_req_count());
	SyncAllLowLevelReqDone();

	TEST_ASSERT_EQUAL_UINT(0, notCompletedNandReqCnt);
	TEST_ASSERT_EQUAL_UINT(1, mock_nsc_count_cmd(V2FCommand_ProgramPage));
	TEST_ASSERT_NOT_EQUAL(VSA_FAIL, AddrTransRead(0));
}

/* ------------------------------------------------------------------------ */
/* DataReadFromNand                                                         */
/* ------------------------------------------------------------------------ */

static void test_data_read_from_nand_unmapped_lsa_does_nothing(void)
{
	unsigned int tag = new_slice_req(REQ_CODE_READ, 8, NVME_BLOCKS_PER_SLICE);

	req(tag)->dataBufInfo.entry = 2;

	DataReadFromNand(tag);

	TEST_ASSERT_EQUAL_UINT(0, total_nand_req_count());
	TEST_ASSERT_EQUAL_UINT(REQ_SLOT_TAG_NONE, dataBufMapPtr->dataBuf[2].blockingReqTail);
}

static void test_data_read_from_nand_mapped_lsa_queues_read_request(void)
{
	unsigned int tag, readTag, vsa;

	vsa = AddrTransWrite(8);
	tag = new_slice_req(REQ_CODE_READ, 8, NVME_BLOCKS_PER_SLICE);
	req(tag)->nvmeCmdSlotTag = 5;
	req(tag)->dataBufInfo.entry = 2;
	/* Mark the page programmed so the read passes the row-address check. */
	dep_entry_for_vsa(vsa)->permittedProgPage = Vsa2VpageTranslation(vsa) + 1;

	DataReadFromNand(tag);

	TEST_ASSERT_EQUAL_UINT(1, nandReqQ[ch_of_vsa(vsa)][way_of_vsa(vsa)].reqCnt);
	readTag = nandReqQ[ch_of_vsa(vsa)][way_of_vsa(vsa)].headReq;
	TEST_ASSERT_EQUAL_UINT(REQ_TYPE_NAND, req(readTag)->reqType);
	TEST_ASSERT_EQUAL_UINT(REQ_CODE_READ, req(readTag)->reqCode);
	TEST_ASSERT_EQUAL_UINT(5, req(readTag)->nvmeCmdSlotTag);
	TEST_ASSERT_EQUAL_UINT(8, req(readTag)->logicalSliceAddr);
	TEST_ASSERT_EQUAL_UINT(vsa, req(readTag)->nandInfo.virtualSliceAddr);
	TEST_ASSERT_EQUAL_UINT(2, req(readTag)->dataBufInfo.entry);
	TEST_ASSERT_EQUAL_UINT(REQ_OPT_BLOCK_SPACE_MAIN, req(readTag)->reqOpt.blockSpace);
	TEST_ASSERT_EQUAL_UINT(readTag, dataBufMapPtr->dataBuf[2].blockingReqTail);
}

static void test_data_read_from_nand_of_unprogrammed_page_is_row_addr_blocked(void)
{
	unsigned int tag, vsa;

	vsa = AddrTransWrite(8);
	tag = new_slice_req(REQ_CODE_READ, 8, NVME_BLOCKS_PER_SLICE);
	req(tag)->dataBufInfo.entry = 2;

	DataReadFromNand(tag);

	TEST_ASSERT_EQUAL_UINT(0, total_nand_req_count());
	TEST_ASSERT_EQUAL_UINT(1, blockedByRowAddrDepReqQ[ch_of_vsa(vsa)][way_of_vsa(vsa)].reqCnt);
	TEST_ASSERT_EQUAL_UINT(1, dep_entry_for_vsa(vsa)->blockedReadReqCnt);
}

/* ------------------------------------------------------------------------ */
/* CheckBufDep / CheckRowAddrDep                                            */
/* ------------------------------------------------------------------------ */

static void test_check_buf_dep_reports_pass_and_blocked(void)
{
	unsigned int tag = new_nand_req(REQ_CODE_READ, 0);

	TEST_ASSERT_EQUAL_UINT(BUF_DEPENDENCY_REPORT_PASS, CheckBufDep(tag));
	req(tag)->prevBlockingReq = 3;
	TEST_ASSERT_EQUAL_UINT(BUF_DEPENDENCY_REPORT_BLOCKED, CheckBufDep(tag));
}

static void test_check_row_addr_dep_read_select_passes_below_permitted_page(void)
{
	unsigned int vsa = Vorg2VsaTranslation(0, 3, 2);
	unsigned int tag = new_nand_req(REQ_CODE_READ, vsa);

	dep_entry_for_vsa(vsa)->permittedProgPage = 3;

	TEST_ASSERT_EQUAL_UINT(ROW_ADDR_DEPENDENCY_REPORT_PASS, CheckRowAddrDep(tag, ROW_ADDR_DEPENDENCY_CHECK_OPT_SELECT));
	TEST_ASSERT_EQUAL_UINT(0, dep_entry_for_vsa(vsa)->blockedReadReqCnt);
}

static void test_check_row_addr_dep_read_select_blocks_and_counts(void)
{
	unsigned int vsa = Vorg2VsaTranslation(0, 3, 2);
	unsigned int tag = new_nand_req(REQ_CODE_READ, vsa);

	dep_entry_for_vsa(vsa)->permittedProgPage = 2;

	TEST_ASSERT_EQUAL_UINT(ROW_ADDR_DEPENDENCY_REPORT_BLOCKED, CheckRowAddrDep(tag, ROW_ADDR_DEPENDENCY_CHECK_OPT_SELECT));
	TEST_ASSERT_EQUAL_UINT(1, dep_entry_for_vsa(vsa)->blockedReadReqCnt);
}

static void test_check_row_addr_dep_read_release_decrements_on_pass(void)
{
	unsigned int vsa = Vorg2VsaTranslation(1, 3, 2);
	unsigned int tag = new_nand_req(REQ_CODE_READ, vsa);

	dep_entry_for_vsa(vsa)->blockedReadReqCnt = 2;
	dep_entry_for_vsa(vsa)->permittedProgPage = 2;
	TEST_ASSERT_EQUAL_UINT(ROW_ADDR_DEPENDENCY_REPORT_BLOCKED, CheckRowAddrDep(tag, ROW_ADDR_DEPENDENCY_CHECK_OPT_RELEASE));
	TEST_ASSERT_EQUAL_UINT(2, dep_entry_for_vsa(vsa)->blockedReadReqCnt);

	dep_entry_for_vsa(vsa)->permittedProgPage = 3;
	TEST_ASSERT_EQUAL_UINT(ROW_ADDR_DEPENDENCY_REPORT_PASS, CheckRowAddrDep(tag, ROW_ADDR_DEPENDENCY_CHECK_OPT_RELEASE));
	TEST_ASSERT_EQUAL_UINT(1, dep_entry_for_vsa(vsa)->blockedReadReqCnt);
}

static void test_check_row_addr_dep_read_rejects_unknown_option(void)
{
	unsigned int tag = new_nand_req(REQ_CODE_READ, 0);

	FW_EXPECT_ASSERT(CheckRowAddrDep(tag, 5));
}

static void test_check_row_addr_dep_write_passes_only_on_next_page(void)
{
	unsigned int vsa = Vorg2VsaTranslation(0, 4, 1);
	unsigned int tag = new_nand_req(REQ_CODE_WRITE, vsa);

	dep_entry_for_vsa(vsa)->permittedProgPage = 0;
	TEST_ASSERT_EQUAL_UINT(ROW_ADDR_DEPENDENCY_REPORT_BLOCKED, CheckRowAddrDep(tag, ROW_ADDR_DEPENDENCY_CHECK_OPT_SELECT));
	TEST_ASSERT_EQUAL_UINT(0, dep_entry_for_vsa(vsa)->permittedProgPage);

	dep_entry_for_vsa(vsa)->permittedProgPage = 1;
	TEST_ASSERT_EQUAL_UINT(ROW_ADDR_DEPENDENCY_REPORT_PASS, CheckRowAddrDep(tag, ROW_ADDR_DEPENDENCY_CHECK_OPT_SELECT));
	TEST_ASSERT_EQUAL_UINT(2, dep_entry_for_vsa(vsa)->permittedProgPage);
}

static void test_check_row_addr_dep_erase_passes_when_block_fully_programmed_and_unread(void)
{
	unsigned int vsa = Vorg2VsaTranslation(0, 6, 0);
	unsigned int tag = new_nand_req(REQ_CODE_ERASE, vsa);

	req(tag)->nandInfo.programmedPageCnt = 4;
	dep_entry_for_vsa(vsa)->permittedProgPage = 4;
	dep_entry_for_vsa(vsa)->blockedReadReqCnt = 0;
	dep_entry_for_vsa(vsa)->blockedEraseReqFlag = 1;

	TEST_ASSERT_EQUAL_UINT(ROW_ADDR_DEPENDENCY_REPORT_PASS, CheckRowAddrDep(tag, ROW_ADDR_DEPENDENCY_CHECK_OPT_SELECT));
	TEST_ASSERT_EQUAL_UINT(0, dep_entry_for_vsa(vsa)->permittedProgPage);
	TEST_ASSERT_EQUAL_UINT(0, dep_entry_for_vsa(vsa)->blockedEraseReqFlag);
}

static void test_check_row_addr_dep_erase_select_blocks_and_sets_flag(void)
{
	unsigned int vsa = Vorg2VsaTranslation(0, 6, 0);
	unsigned int tag = new_nand_req(REQ_CODE_ERASE, vsa);

	req(tag)->nandInfo.programmedPageCnt = 4;
	dep_entry_for_vsa(vsa)->permittedProgPage = 4;
	dep_entry_for_vsa(vsa)->blockedReadReqCnt = 1;

	TEST_ASSERT_EQUAL_UINT(ROW_ADDR_DEPENDENCY_REPORT_BLOCKED, CheckRowAddrDep(tag, ROW_ADDR_DEPENDENCY_CHECK_OPT_SELECT));
	TEST_ASSERT_EQUAL_UINT(1, dep_entry_for_vsa(vsa)->blockedEraseReqFlag);
}

static void test_check_row_addr_dep_erase_release_blocks_without_touching_flag(void)
{
	unsigned int vsa = Vorg2VsaTranslation(0, 6, 0);
	unsigned int tag = new_nand_req(REQ_CODE_ERASE, vsa);

	req(tag)->nandInfo.programmedPageCnt = 4;
	dep_entry_for_vsa(vsa)->permittedProgPage = 2;
	dep_entry_for_vsa(vsa)->blockedEraseReqFlag = 0;

	TEST_ASSERT_EQUAL_UINT(ROW_ADDR_DEPENDENCY_REPORT_BLOCKED, CheckRowAddrDep(tag, ROW_ADDR_DEPENDENCY_CHECK_OPT_RELEASE));
	TEST_ASSERT_EQUAL_UINT(0, dep_entry_for_vsa(vsa)->blockedEraseReqFlag);
}

static void test_check_row_addr_dep_erase_rejects_unknown_option(void)
{
	unsigned int tag = new_nand_req(REQ_CODE_ERASE, 0);

	req(tag)->nandInfo.programmedPageCnt = 1;
	FW_EXPECT_ASSERT(CheckRowAddrDep(tag, 5));
}

static void test_check_row_addr_dep_rejects_physical_addressing(void)
{
	unsigned int tag = new_nand_req(REQ_CODE_READ, 0);

	req(tag)->reqOpt.nandAddr = REQ_OPT_NAND_ADDR_PHY_ORG;
	FW_EXPECT_ASSERT(CheckRowAddrDep(tag, ROW_ADDR_DEPENDENCY_CHECK_OPT_SELECT));
}

static void test_check_row_addr_dep_rejects_unknown_req_code(void)
{
	unsigned int tag = new_nand_req(REQ_CODE_RESET, 0);

	FW_EXPECT_ASSERT(CheckRowAddrDep(tag, ROW_ADDR_DEPENDENCY_CHECK_OPT_SELECT));
}

/* ------------------------------------------------------------------------ */
/* UpdateRowAddrDepTableForBufBlockedReq                                    */
/* ------------------------------------------------------------------------ */

static void test_update_dep_table_buf_blocked_read_counts_blocked_read(void)
{
	unsigned int vsa = Vorg2VsaTranslation(0, 7, 0);
	unsigned int tag = new_nand_req(REQ_CODE_READ, vsa);

	req(tag)->prevBlockingReq = 1;

	TEST_ASSERT_EQUAL_UINT(ROW_ADDR_DEPENDENCY_TABLE_UPDATE_REPORT_DONE, UpdateRowAddrDepTableForBufBlockedReq(tag));
	TEST_ASSERT_EQUAL_UINT(1, dep_entry_for_vsa(vsa)->blockedReadReqCnt);
}

static void test_update_dep_table_buf_blocked_erase_sets_flag(void)
{
	unsigned int vsa = Vorg2VsaTranslation(0, 7, 0);
	unsigned int tag = new_nand_req(REQ_CODE_ERASE, vsa);

	TEST_ASSERT_EQUAL_UINT(ROW_ADDR_DEPENDENCY_TABLE_UPDATE_REPORT_DONE, UpdateRowAddrDepTableForBufBlockedReq(tag));
	TEST_ASSERT_EQUAL_UINT(1, dep_entry_for_vsa(vsa)->blockedEraseReqFlag);
}

static void test_update_dep_table_buf_blocked_write_leaves_table_untouched(void)
{
	unsigned int vsa = Vorg2VsaTranslation(0, 7, 0);
	unsigned int tag = new_nand_req(REQ_CODE_WRITE, vsa);

	TEST_ASSERT_EQUAL_UINT(ROW_ADDR_DEPENDENCY_TABLE_UPDATE_REPORT_DONE, UpdateRowAddrDepTableForBufBlockedReq(tag));
	TEST_ASSERT_EQUAL_UINT(0, dep_entry_for_vsa(vsa)->blockedReadReqCnt);
	TEST_ASSERT_EQUAL_UINT(0, dep_entry_for_vsa(vsa)->blockedEraseReqFlag);
	TEST_ASSERT_EQUAL_UINT(0, dep_entry_for_vsa(vsa)->permittedProgPage);
}

static void test_update_dep_table_rejects_physical_addressing(void)
{
	unsigned int tag = new_nand_req(REQ_CODE_READ, 0);

	req(tag)->reqOpt.nandAddr = REQ_OPT_NAND_ADDR_PHY_ORG;
	FW_EXPECT_ASSERT(UpdateRowAddrDepTableForBufBlockedReq(tag));
}

/*
 * A read that is buffer-blocked while an erase of the same block is pending
 * forces the erase to complete synchronously (SyncReleaseEraseReq). The
 * blocked erase can only complete once the block is fully programmed, so a
 * write to the missing page is queued first and the scheduler drains both.
 */
static void run_sync_release_scenario(unsigned int readPage, unsigned int *readTag, unsigned int *vsaOut)
{
	unsigned int blockNo = 9, dieNo = 0;
	unsigned int eraseVsa = Vorg2VsaTranslation(dieNo, blockNo, 0);
	unsigned int eraseTag = new_nand_req(REQ_CODE_ERASE, eraseVsa);
	unsigned int writeTag = new_nand_req(REQ_CODE_WRITE, Vorg2VsaTranslation(dieNo, blockNo, 0));
	unsigned int ch = ch_of_vsa(eraseVsa), way = way_of_vsa(eraseVsa);

	/* Erase wants one programmed page but none is yet: blocked, flag set. */
	req(eraseTag)->nandInfo.programmedPageCnt = 1;
	SelectLowLevelReqQ(eraseTag);
	TEST_ASSERT_EQUAL_UINT(1, blockedByRowAddrDepReqQ[ch][way].reqCnt);
	TEST_ASSERT_EQUAL_UINT(1, dep_entry_for_vsa(eraseVsa)->blockedEraseReqFlag);

	/* Program page 0 so the erase becomes eligible on the next release pass. */
	SelectLowLevelReqQ(writeTag);
	TEST_ASSERT_EQUAL_UINT(1, nandReqQ[ch][way].reqCnt);

	*vsaOut = Vorg2VsaTranslation(dieNo, blockNo, readPage);
	*readTag = new_nand_req(REQ_CODE_READ, *vsaOut);
	/* Buffer-blocked by a request that has already left the DMA queue. */
	req(*readTag)->prevBlockingReq = writeTag;
	req(*readTag)->reqOpt.dataBufFormat = REQ_OPT_DATA_BUF_NONE;
}

static void test_update_dep_table_sync_release_then_read_becomes_row_addr_blocked(void)
{
	unsigned int readTag, vsa, ch, way;

	run_sync_release_scenario(0, &readTag, &vsa);
	ch = ch_of_vsa(vsa);
	way = way_of_vsa(vsa);

	/* Simulate the blocking request having completed: the read is now buf-free. */
	req(readTag)->prevBlockingReq = REQ_SLOT_TAG_NONE;

	TEST_ASSERT_EQUAL_UINT(ROW_ADDR_DEPENDENCY_TABLE_UPDATE_REPORT_SYNC, UpdateRowAddrDepTableForBufBlockedReq(readTag));

	/* Erase drained: block reset to page 0, so a read of page 0 is now blocked. */
	TEST_ASSERT_EQUAL_UINT(0, dep_entry_for_vsa(vsa)->blockedEraseReqFlag);
	TEST_ASSERT_EQUAL_UINT(0, dep_entry_for_vsa(vsa)->permittedProgPage);
	TEST_ASSERT_EQUAL_UINT(1, mock_nsc_count_cmd(V2FCommand_BlockErase));
	TEST_ASSERT_EQUAL_UINT(1, mock_nsc_count_cmd(V2FCommand_ProgramPage));
	TEST_ASSERT_EQUAL_UINT(1, blockedByRowAddrDepReqQ[ch][way].reqCnt);
	TEST_ASSERT_EQUAL_UINT(1, dep_entry_for_vsa(vsa)->blockedReadReqCnt);
	TEST_ASSERT_EQUAL_UINT(REQ_QUEUE_TYPE_BLOCKED_BY_ROW_ADDR_DEP, req(readTag)->reqQueueType);
}

static void test_update_dep_table_sync_release_then_read_goes_to_nand_queue(void)
{
	unsigned int readTag, vsa, ch, way, writeTag2;

	run_sync_release_scenario(0, &readTag, &vsa);
	ch = ch_of_vsa(vsa);
	way = way_of_vsa(vsa);

	/* A second write to page 0 is out of order now (page 1 is next), so it
	 * waits behind the erase and re-programs page 0 once the block is reset. */
	writeTag2 = new_nand_req(REQ_CODE_WRITE, Vorg2VsaTranslation(0, 9, 0));
	SelectLowLevelReqQ(writeTag2);
	TEST_ASSERT_EQUAL_UINT(2, blockedByRowAddrDepReqQ[ch][way].reqCnt);

	req(readTag)->prevBlockingReq = REQ_SLOT_TAG_NONE;

	TEST_ASSERT_EQUAL_UINT(ROW_ADDR_DEPENDENCY_TABLE_UPDATE_REPORT_SYNC, UpdateRowAddrDepTableForBufBlockedReq(readTag));

	TEST_ASSERT_EQUAL_UINT(0, dep_entry_for_vsa(vsa)->blockedEraseReqFlag);
	TEST_ASSERT_EQUAL_UINT(1, dep_entry_for_vsa(vsa)->permittedProgPage);
	TEST_ASSERT_EQUAL_UINT(1, mock_nsc_count_cmd(V2FCommand_BlockErase));
	/* Page 0 is programmed again, so the read goes straight to the NAND queue. */
	TEST_ASSERT_EQUAL_UINT(REQ_QUEUE_TYPE_NAND, req(readTag)->reqQueueType);
	TEST_ASSERT_EQUAL_UINT(0, dep_entry_for_vsa(vsa)->blockedReadReqCnt);
	TEST_ASSERT_EQUAL_UINT(0, blockedByRowAddrDepReqQ[ch][way].reqCnt);
}

static void test_update_dep_table_sync_release_still_buf_blocked_counts_read(void)
{
	unsigned int readTag, vsa;

	run_sync_release_scenario(0, &readTag, &vsa);

	/* prevBlockingReq still set: after the sync the read stays buf-blocked. */
	TEST_ASSERT_EQUAL_UINT(ROW_ADDR_DEPENDENCY_TABLE_UPDATE_REPORT_DONE, UpdateRowAddrDepTableForBufBlockedReq(readTag));
	TEST_ASSERT_EQUAL_UINT(0, dep_entry_for_vsa(vsa)->blockedEraseReqFlag);
	TEST_ASSERT_EQUAL_UINT(1, dep_entry_for_vsa(vsa)->blockedReadReqCnt);
	TEST_ASSERT_EQUAL_UINT(1, mock_nsc_count_cmd(V2FCommand_BlockErase));
}

/* ------------------------------------------------------------------------ */
/* SelectLowLevelReqQ                                                       */
/* ------------------------------------------------------------------------ */

static void test_select_low_level_nand_read_pass_goes_to_nand_queue(void)
{
	unsigned int vsa = Vorg2VsaTranslation(1, 2, 0);
	unsigned int tag = new_nand_req(REQ_CODE_READ, vsa);

	dep_entry_for_vsa(vsa)->permittedProgPage = 1;

	SelectLowLevelReqQ(tag);

	TEST_ASSERT_EQUAL_UINT(1, nandReqQ[ch_of_vsa(vsa)][way_of_vsa(vsa)].reqCnt);
	TEST_ASSERT_EQUAL_UINT(REQ_QUEUE_TYPE_NAND, req(tag)->reqQueueType);
	TEST_ASSERT_EQUAL_UINT(1, notCompletedNandReqCnt);
}

static void test_select_low_level_nand_read_blocked_goes_to_row_addr_queue(void)
{
	unsigned int vsa = Vorg2VsaTranslation(1, 2, 0);
	unsigned int tag = new_nand_req(REQ_CODE_READ, vsa);

	SelectLowLevelReqQ(tag);

	TEST_ASSERT_EQUAL_UINT(0, total_nand_req_count());
	TEST_ASSERT_EQUAL_UINT(1, blockedByRowAddrDepReqQ[ch_of_vsa(vsa)][way_of_vsa(vsa)].reqCnt);
	TEST_ASSERT_EQUAL_UINT(REQ_QUEUE_TYPE_BLOCKED_BY_ROW_ADDR_DEP, req(tag)->reqQueueType);
}

static void test_select_low_level_nand_without_dependency_check_skips_table(void)
{
	unsigned int vsa = Vorg2VsaTranslation(1, 2, 0);
	unsigned int tag = new_nand_req(REQ_CODE_READ, vsa);

	req(tag)->reqOpt.rowAddrDependencyCheck = REQ_OPT_ROW_ADDR_DEPENDENCY_NONE;

	SelectLowLevelReqQ(tag);

	TEST_ASSERT_EQUAL_UINT(1, nandReqQ[ch_of_vsa(vsa)][way_of_vsa(vsa)].reqCnt);
	TEST_ASSERT_EQUAL_UINT(0, dep_entry_for_vsa(vsa)->blockedReadReqCnt);
}

static void test_select_low_level_physical_address_uses_ch_way_fields(void)
{
	unsigned int tag = new_nand_req(REQ_CODE_ERASE, 0);

	req(tag)->reqOpt.nandAddr = REQ_OPT_NAND_ADDR_PHY_ORG;
	req(tag)->reqOpt.rowAddrDependencyCheck = REQ_OPT_ROW_ADDR_DEPENDENCY_NONE;
	req(tag)->reqOpt.blockSpace = REQ_OPT_BLOCK_SPACE_TOTAL;
	req(tag)->nandInfo.physicalCh = USER_CHANNELS - 1;
	req(tag)->nandInfo.physicalWay = USER_WAYS - 1;
	req(tag)->nandInfo.physicalBlock = 3;

	SelectLowLevelReqQ(tag);

	TEST_ASSERT_EQUAL_UINT(1, nandReqQ[USER_CHANNELS - 1][USER_WAYS - 1].reqCnt);
}

static void test_select_low_level_rejects_bad_nand_addr_option(void)
{
	unsigned int tag = new_nand_req(REQ_CODE_READ, 0);

	req(tag)->reqOpt.nandAddr = 2;
	FW_EXPECT_ASSERT(SelectLowLevelReqQ(tag));
}

static void test_select_low_level_rejects_unknown_req_type(void)
{
	unsigned int tag = new_nand_req(REQ_CODE_READ, 0);

	req(tag)->reqType = REQ_TYPE_SLICE;
	FW_EXPECT_ASSERT(SelectLowLevelReqQ(tag));
}

static void test_select_low_level_nvme_dma_is_issued_immediately(void)
{
	unsigned int tag = new_slice_req(REQ_CODE_TxDMA, 0, 2);

	req(tag)->reqType = REQ_TYPE_NVME_DMA;
	req(tag)->reqOpt.dataBufFormat = REQ_OPT_DATA_BUF_ENTRY;
	req(tag)->dataBufInfo.entry = 0;

	SelectLowLevelReqQ(tag);

	TEST_ASSERT_EQUAL_UINT(1, nvmeDmaReqQ.reqCnt);
	TEST_ASSERT_EQUAL_UINT(2, mock_host_count(MOCK_HOST_SET_AUTO_TX_DMA));
	TEST_ASSERT_EQUAL_UINT(REQ_QUEUE_TYPE_NVME_DMA, req(tag)->reqQueueType);
}

static void test_select_low_level_buf_blocked_nvme_dma_waits_in_buf_dep_queue(void)
{
	unsigned int tag = new_slice_req(REQ_CODE_TxDMA, 0, 2);

	req(tag)->reqType = REQ_TYPE_NVME_DMA;
	req(tag)->reqOpt.dataBufFormat = REQ_OPT_DATA_BUF_ENTRY;
	req(tag)->prevBlockingReq = 1;

	SelectLowLevelReqQ(tag);

	TEST_ASSERT_EQUAL_UINT(0, nvmeDmaReqQ.reqCnt);
	TEST_ASSERT_EQUAL_UINT(1, blockedByBufDepReqQ.reqCnt);
	TEST_ASSERT_EQUAL_UINT(0, mock_host_count(MOCK_HOST_SET_AUTO_TX_DMA));
}

static void test_select_low_level_buf_blocked_nand_read_updates_dependency_table(void)
{
	unsigned int vsa = Vorg2VsaTranslation(0, 2, 0);
	unsigned int tag = new_nand_req(REQ_CODE_READ, vsa);

	req(tag)->prevBlockingReq = 1;

	SelectLowLevelReqQ(tag);

	TEST_ASSERT_EQUAL_UINT(1, blockedByBufDepReqQ.reqCnt);
	TEST_ASSERT_EQUAL_UINT(1, dep_entry_for_vsa(vsa)->blockedReadReqCnt);
}

static void test_select_low_level_buf_blocked_nand_without_check_skips_table(void)
{
	unsigned int vsa = Vorg2VsaTranslation(0, 2, 0);
	unsigned int tag = new_nand_req(REQ_CODE_READ, vsa);

	req(tag)->prevBlockingReq = 1;
	req(tag)->reqOpt.rowAddrDependencyCheck = REQ_OPT_ROW_ADDR_DEPENDENCY_NONE;

	SelectLowLevelReqQ(tag);

	TEST_ASSERT_EQUAL_UINT(1, blockedByBufDepReqQ.reqCnt);
	TEST_ASSERT_EQUAL_UINT(0, dep_entry_for_vsa(vsa)->blockedReadReqCnt);
}

/* ------------------------------------------------------------------------ */
/* ReleaseBlockedByBufDepReq                                                */
/* ------------------------------------------------------------------------ */

static void test_release_buf_dep_with_no_successor_clears_entry_tail(void)
{
	unsigned int tag = new_nand_req(REQ_CODE_WRITE, 0);

	req(tag)->reqOpt.dataBufFormat = REQ_OPT_DATA_BUF_ENTRY;
	req(tag)->dataBufInfo.entry = 4;
	dataBufMapPtr->dataBuf[4].blockingReqTail = tag;

	ReleaseBlockedByBufDepReq(tag);

	TEST_ASSERT_EQUAL_UINT(REQ_SLOT_TAG_NONE, dataBufMapPtr->dataBuf[4].blockingReqTail);
}

static void test_release_buf_dep_keeps_tail_owned_by_another_request(void)
{
	unsigned int tag = new_nand_req(REQ_CODE_WRITE, 0);

	req(tag)->reqOpt.dataBufFormat = REQ_OPT_DATA_BUF_ENTRY;
	req(tag)->dataBufInfo.entry = 4;
	dataBufMapPtr->dataBuf[4].blockingReqTail = 77;

	ReleaseBlockedByBufDepReq(tag);

	TEST_ASSERT_EQUAL_UINT(77, dataBufMapPtr->dataBuf[4].blockingReqTail);
}

static void test_release_buf_dep_clears_temp_entry_tail(void)
{
	unsigned int tag = new_nand_req(REQ_CODE_WRITE, 0);

	req(tag)->reqOpt.dataBufFormat = REQ_OPT_DATA_BUF_TEMP_ENTRY;
	req(tag)->dataBufInfo.entry = 0;
	tempDataBufMapPtr->tempDataBuf[0].blockingReqTail = tag;

	ReleaseBlockedByBufDepReq(tag);

	TEST_ASSERT_EQUAL_UINT(REQ_SLOT_TAG_NONE, tempDataBufMapPtr->tempDataBuf[0].blockingReqTail);
}

static void test_release_buf_dep_unblocks_successor_nvme_dma(void)
{
	unsigned int first, second;

	ReqTransNvmeToSlice(0, 0, FULL_SLICE_NLB, IO_NVM_WRITE);
	first = sliceReqQ.headReq;
	ReqTransSliceToLowLevel();
	ReqTransNvmeToSlice(1, 0, FULL_SLICE_NLB, IO_NVM_READ);
	second = sliceReqQ.headReq;
	ReqTransSliceToLowLevel();
	TEST_ASSERT_EQUAL_UINT(1, blockedByBufDepReqQ.reqCnt);
	TEST_ASSERT_EQUAL_UINT(0, mock_host_count(MOCK_HOST_SET_AUTO_TX_DMA));

	ReleaseBlockedByBufDepReq(first);

	TEST_ASSERT_EQUAL_UINT(REQ_SLOT_TAG_NONE, req(first)->nextBlockingReq);
	TEST_ASSERT_EQUAL_UINT(REQ_SLOT_TAG_NONE, req(second)->prevBlockingReq);
	TEST_ASSERT_EQUAL_UINT(0, blockedByBufDepReqQ.reqCnt);
	TEST_ASSERT_EQUAL_UINT(REQ_QUEUE_TYPE_NVME_DMA, req(second)->reqQueueType);
	TEST_ASSERT_EQUAL_UINT(NVME_BLOCKS_PER_SLICE, mock_host_count(MOCK_HOST_SET_AUTO_TX_DMA));
	/* The chain tail is still the second request, so it stays. */
	TEST_ASSERT_EQUAL_UINT(second, dataBufMapPtr->dataBuf[req(second)->dataBufInfo.entry].blockingReqTail);
}

static void test_release_buf_dep_unblocks_successor_nand_write(void)
{
	unsigned int vsa = Vorg2VsaTranslation(0, 3, 0);
	unsigned int first = new_nand_req(REQ_CODE_READ, vsa);
	unsigned int second = new_nand_req(REQ_CODE_WRITE, vsa);

	req(first)->reqOpt.dataBufFormat = REQ_OPT_DATA_BUF_ENTRY;
	req(first)->dataBufInfo.entry = 1;
	req(second)->reqOpt.dataBufFormat = REQ_OPT_DATA_BUF_ENTRY;
	req(second)->dataBufInfo.entry = 1;
	req(first)->nextBlockingReq = second;
	req(second)->prevBlockingReq = first;
	dataBufMapPtr->dataBuf[1].blockingReqTail = second;
	SelectLowLevelReqQ(second);
	TEST_ASSERT_EQUAL_UINT(1, blockedByBufDepReqQ.reqCnt);

	ReleaseBlockedByBufDepReq(first);

	TEST_ASSERT_EQUAL_UINT(0, blockedByBufDepReqQ.reqCnt);
	TEST_ASSERT_EQUAL_UINT(1, nandReqQ[ch_of_vsa(vsa)][way_of_vsa(vsa)].reqCnt);
	TEST_ASSERT_EQUAL_UINT(1, dep_entry_for_vsa(vsa)->permittedProgPage);
}

static void test_release_buf_dep_successor_nand_read_still_row_addr_blocked(void)
{
	unsigned int vsa = Vorg2VsaTranslation(0, 3, 0);
	unsigned int first = new_nand_req(REQ_CODE_WRITE, vsa);
	unsigned int second = new_nand_req(REQ_CODE_READ, vsa);

	req(second)->prevBlockingReq = first;
	req(first)->nextBlockingReq = second;
	SelectLowLevelReqQ(second);
	TEST_ASSERT_EQUAL_UINT(1, dep_entry_for_vsa(vsa)->blockedReadReqCnt);

	ReleaseBlockedByBufDepReq(first);

	TEST_ASSERT_EQUAL_UINT(0, blockedByBufDepReqQ.reqCnt);
	TEST_ASSERT_EQUAL_UINT(1, blockedByRowAddrDepReqQ[ch_of_vsa(vsa)][way_of_vsa(vsa)].reqCnt);
	TEST_ASSERT_EQUAL_UINT(1, dep_entry_for_vsa(vsa)->blockedReadReqCnt);
}

static void test_release_buf_dep_successor_nand_read_passes_when_page_programmed(void)
{
	unsigned int vsa = Vorg2VsaTranslation(0, 3, 0);
	unsigned int first = new_nand_req(REQ_CODE_WRITE, vsa);
	unsigned int second = new_nand_req(REQ_CODE_READ, vsa);

	req(second)->prevBlockingReq = first;
	req(first)->nextBlockingReq = second;
	SelectLowLevelReqQ(second);
	dep_entry_for_vsa(vsa)->permittedProgPage = 1;

	ReleaseBlockedByBufDepReq(first);

	TEST_ASSERT_EQUAL_UINT(1, nandReqQ[ch_of_vsa(vsa)][way_of_vsa(vsa)].reqCnt);
	TEST_ASSERT_EQUAL_UINT(0, dep_entry_for_vsa(vsa)->blockedReadReqCnt);
}

static void test_release_buf_dep_successor_without_dependency_check_goes_to_nand(void)
{
	unsigned int vsa = Vorg2VsaTranslation(0, 3, 0);
	unsigned int first = new_nand_req(REQ_CODE_WRITE, vsa);
	unsigned int second = new_nand_req(REQ_CODE_READ, vsa);

	req(second)->reqOpt.rowAddrDependencyCheck = REQ_OPT_ROW_ADDR_DEPENDENCY_NONE;
	req(second)->prevBlockingReq = first;
	req(first)->nextBlockingReq = second;
	SelectLowLevelReqQ(second);

	ReleaseBlockedByBufDepReq(first);

	TEST_ASSERT_EQUAL_UINT(1, nandReqQ[ch_of_vsa(vsa)][way_of_vsa(vsa)].reqCnt);
}

static void test_release_buf_dep_successor_not_in_buf_dep_queue_is_only_unlinked(void)
{
	unsigned int first = new_nand_req(REQ_CODE_WRITE, 0);
	unsigned int second = new_nand_req(REQ_CODE_READ, 0);

	req(second)->prevBlockingReq = first;
	req(first)->nextBlockingReq = second;
	req(second)->reqQueueType = REQ_QUEUE_TYPE_NAND;

	ReleaseBlockedByBufDepReq(first);

	TEST_ASSERT_EQUAL_UINT(REQ_SLOT_TAG_NONE, req(second)->prevBlockingReq);
	TEST_ASSERT_EQUAL_UINT(0, total_nand_req_count());
}

static void test_release_buf_dep_successor_with_physical_address_asserts(void)
{
	unsigned int first = new_nand_req(REQ_CODE_WRITE, 0);
	unsigned int second = new_nand_req(REQ_CODE_READ, 0);

	req(second)->prevBlockingReq = first;
	req(first)->nextBlockingReq = second;
	SelectLowLevelReqQ(second);
	req(second)->reqOpt.nandAddr = REQ_OPT_NAND_ADDR_PHY_ORG;

	FW_EXPECT_ASSERT(ReleaseBlockedByBufDepReq(first));
}

/* ------------------------------------------------------------------------ */
/* ReleaseBlockedByRowAddrDepReq                                            */
/* ------------------------------------------------------------------------ */

static void test_release_row_addr_dep_moves_released_reads_and_keeps_blocked_ones(void)
{
	unsigned int vsaPage0 = Vorg2VsaTranslation(0, 5, 0);
	unsigned int vsaPage1 = Vorg2VsaTranslation(0, 5, 1);
	unsigned int readPage0 = new_nand_req(REQ_CODE_READ, vsaPage0);
	unsigned int readPage1 = new_nand_req(REQ_CODE_READ, vsaPage1);
	unsigned int ch = ch_of_vsa(vsaPage0), way = way_of_vsa(vsaPage0);

	SelectLowLevelReqQ(readPage0);
	SelectLowLevelReqQ(readPage1);
	TEST_ASSERT_EQUAL_UINT(2, blockedByRowAddrDepReqQ[ch][way].reqCnt);
	TEST_ASSERT_EQUAL_UINT(2, dep_entry_for_vsa(vsaPage0)->blockedReadReqCnt);

	/* Nothing programmed yet: both remain blocked. */
	ReleaseBlockedByRowAddrDepReq(ch, way);
	TEST_ASSERT_EQUAL_UINT(2, blockedByRowAddrDepReqQ[ch][way].reqCnt);

	/* Page 0 programmed: only the page-0 read is released. */
	dep_entry_for_vsa(vsaPage0)->permittedProgPage = 1;
	ReleaseBlockedByRowAddrDepReq(ch, way);

	TEST_ASSERT_EQUAL_UINT(1, blockedByRowAddrDepReqQ[ch][way].reqCnt);
	TEST_ASSERT_EQUAL_UINT(readPage1, blockedByRowAddrDepReqQ[ch][way].headReq);
	TEST_ASSERT_EQUAL_UINT(1, nandReqQ[ch][way].reqCnt);
	TEST_ASSERT_EQUAL_UINT(readPage0, nandReqQ[ch][way].headReq);
	TEST_ASSERT_EQUAL_UINT(1, dep_entry_for_vsa(vsaPage0)->blockedReadReqCnt);
}

static void test_release_row_addr_dep_on_empty_queue_is_noop(void)
{
	ReleaseBlockedByRowAddrDepReq(0, 0);

	TEST_ASSERT_EQUAL_UINT(0, total_nand_req_count());
}

static void test_release_row_addr_dep_rejects_request_without_dependency_check(void)
{
	unsigned int tag = new_nand_req(REQ_CODE_READ, 0);

	PutToBlockedByRowAddrDepReqQ(tag, 0, 0);
	req(tag)->reqOpt.rowAddrDependencyCheck = REQ_OPT_ROW_ADDR_DEPENDENCY_NONE;

	FW_EXPECT_ASSERT(ReleaseBlockedByRowAddrDepReq(0, 0));
}

/* ------------------------------------------------------------------------ */
/* IssueNvmeDmaReq / CheckDoneNvmeDmaReq                                    */
/* ------------------------------------------------------------------------ */

static unsigned int new_dma_req(unsigned int reqCode, unsigned int entry, unsigned int startIndex,
		unsigned int blockOffset, unsigned int numOfNvmeBlock)
{
	unsigned int tag = new_slice_req(reqCode, 0, numOfNvmeBlock);

	req(tag)->reqType = REQ_TYPE_NVME_DMA;
	req(tag)->reqOpt.dataBufFormat = REQ_OPT_DATA_BUF_ENTRY;
	req(tag)->dataBufInfo.entry = entry;
	req(tag)->nvmeDmaInfo.startIndex = startIndex;
	req(tag)->nvmeDmaInfo.nvmeBlockOffset = blockOffset;
	return tag;
}

static void test_issue_rx_dma_programs_one_transfer_per_block(void)
{
	unsigned int tag = new_dma_req(REQ_CODE_RxDMA, 2, 5, 1, 3);
	const mock_host_call_t *c;
	unsigned int base = DATA_BUFFER_BASE_ADDR + 2 * BYTES_PER_DATA_REGION_OF_SLICE + 1 * BYTES_PER_NVME_BLOCK;

	req(tag)->nvmeCmdSlotTag = 11;

	IssueNvmeDmaReq(tag);

	TEST_ASSERT_EQUAL_UINT(3, mock_host_count(MOCK_HOST_SET_AUTO_RX_DMA));
	TEST_ASSERT_EQUAL_UINT(0, mock_host_count(MOCK_HOST_SET_AUTO_TX_DMA));
	c = mock_host_call_at(0);
	TEST_ASSERT_EQUAL_UINT(MOCK_HOST_SET_AUTO_RX_DMA, c->kind);
	TEST_ASSERT_EQUAL_UINT(11, c->args[0]);
	TEST_ASSERT_EQUAL_UINT(5, c->args[1]);
	TEST_ASSERT_EQUAL_HEX32(base, c->args[2]);
	c = mock_host_call_at(2);
	TEST_ASSERT_EQUAL_UINT(7, c->args[1]);
	TEST_ASSERT_EQUAL_HEX32(base + 2 * BYTES_PER_NVME_BLOCK, c->args[2]);

	TEST_ASSERT_EQUAL_UINT(g_hostDmaStatus.fifoTail.autoDmaRx, req(tag)->nvmeDmaInfo.reqTail);
	TEST_ASSERT_EQUAL_UINT(g_hostDmaAssistStatus.autoDmaRxOverFlowCnt, req(tag)->nvmeDmaInfo.overFlowCnt);
}

static void test_issue_tx_dma_records_tx_fifo_tail(void)
{
	unsigned int tag = new_dma_req(REQ_CODE_TxDMA, 0, 0, 0, 2);
	const mock_host_call_t *c;

	IssueNvmeDmaReq(tag);

	TEST_ASSERT_EQUAL_UINT(2, mock_host_count(MOCK_HOST_SET_AUTO_TX_DMA));
	c = mock_host_call_at(1);
	TEST_ASSERT_EQUAL_UINT(MOCK_HOST_SET_AUTO_TX_DMA, c->kind);
	TEST_ASSERT_EQUAL_UINT(1, c->args[1]);
	TEST_ASSERT_EQUAL_HEX32(DATA_BUFFER_BASE_ADDR + BYTES_PER_NVME_BLOCK, c->args[2]);
	TEST_ASSERT_EQUAL_UINT(g_hostDmaStatus.fifoTail.autoDmaTx, req(tag)->nvmeDmaInfo.reqTail);
	TEST_ASSERT_EQUAL_UINT(g_hostDmaAssistStatus.autoDmaTxOverFlowCnt, req(tag)->nvmeDmaInfo.overFlowCnt);
}

static void test_issue_dma_with_zero_blocks_programs_nothing(void)
{
	unsigned int tag = new_dma_req(REQ_CODE_TxDMA, 0, 0, 0, 0);

	IssueNvmeDmaReq(tag);

	TEST_ASSERT_EQUAL_UINT(0, mock_host_call_count());
}

static void test_issue_dma_rejects_non_dma_req_code(void)
{
	unsigned int tag = new_dma_req(REQ_CODE_READ, 0, 0, 0, 1);

	FW_EXPECT_ASSERT(IssueNvmeDmaReq(tag));
}

static void test_check_done_dma_with_empty_queue_is_noop(void)
{
	CheckDoneNvmeDmaReq();

	TEST_ASSERT_EQUAL_UINT(0, nvmeDmaReqQ.reqCnt);
}

static void test_check_done_dma_retires_completed_rx_and_tx_requests(void)
{
	unsigned int rx = new_dma_req(REQ_CODE_RxDMA, 0, 0, 0, 1);
	unsigned int tx = new_dma_req(REQ_CODE_TxDMA, 1, 0, 0, 1);

	SelectLowLevelReqQ(rx);
	SelectLowLevelReqQ(tx);
	TEST_ASSERT_EQUAL_UINT(2, nvmeDmaReqQ.reqCnt);

	CheckDoneNvmeDmaReq();

	TEST_ASSERT_EQUAL_UINT(0, nvmeDmaReqQ.reqCnt);
	TEST_ASSERT_EQUAL_UINT(REQ_QUEUE_TYPE_FREE, req(rx)->reqQueueType);
	TEST_ASSERT_EQUAL_UINT(REQ_QUEUE_TYPE_FREE, req(tx)->reqQueueType);
}

static void test_check_done_dma_keeps_requests_while_hardware_is_busy(void)
{
	unsigned int rx = new_dma_req(REQ_CODE_RxDMA, 0, 0, 0, 1);
	unsigned int tx = new_dma_req(REQ_CODE_TxDMA, 1, 0, 0, 1);

	SelectLowLevelReqQ(rx);
	SelectLowLevelReqQ(tx);
	mock_host_set_partial_done(0);

	CheckDoneNvmeDmaReq();

	TEST_ASSERT_EQUAL_UINT(2, nvmeDmaReqQ.reqCnt);
	TEST_ASSERT_EQUAL_UINT(REQ_QUEUE_TYPE_NVME_DMA, req(rx)->reqQueueType);
	TEST_ASSERT_EQUAL_UINT(REQ_QUEUE_TYPE_NVME_DMA, req(tx)->reqQueueType);
}

static void test_check_done_dma_releases_buffer_chain_of_completed_request(void)
{
	unsigned int first, second;

	ReqTransNvmeToSlice(0, 0, FULL_SLICE_NLB, IO_NVM_WRITE);
	first = sliceReqQ.headReq;
	ReqTransSliceToLowLevel();
	ReqTransNvmeToSlice(1, 0, FULL_SLICE_NLB, IO_NVM_READ);
	second = sliceReqQ.headReq;
	ReqTransSliceToLowLevel();
	TEST_ASSERT_EQUAL_UINT(REQ_QUEUE_TYPE_BLOCKED_BY_BUF_DEP, req(second)->reqQueueType);

	CheckDoneNvmeDmaReq();

	/* The RxDMA retired, which released the TxDMA into the DMA queue. */
	TEST_ASSERT_EQUAL_UINT(REQ_QUEUE_TYPE_FREE, req(first)->reqQueueType);
	TEST_ASSERT_EQUAL_UINT(REQ_QUEUE_TYPE_NVME_DMA, req(second)->reqQueueType);
	TEST_ASSERT_EQUAL_UINT(1, nvmeDmaReqQ.reqCnt);
	TEST_ASSERT_EQUAL_UINT(NVME_BLOCKS_PER_SLICE, mock_host_count(MOCK_HOST_SET_AUTO_TX_DMA));
}

int main(void)
{
	UNITY_BEGIN();

	RUN_TEST(test_init_dependency_table_clears_every_entry);

	RUN_TEST(test_nvme_to_slice_single_block_read);
	RUN_TEST(test_nvme_to_slice_aligned_two_full_slices);
	RUN_TEST(test_nvme_to_slice_unaligned_start_within_one_slice);
	RUN_TEST(test_nvme_to_slice_unaligned_start_spanning_two_slices);
	RUN_TEST(test_nvme_to_slice_unaligned_start_with_middle_and_partial_tail);
	RUN_TEST(test_nvme_to_slice_aligned_start_with_partial_tail);
	RUN_TEST(test_nvme_to_slice_unaligned_end_exactly_on_boundary_has_no_tail);
	RUN_TEST(test_nvme_to_slice_rejects_unknown_command_code);

	RUN_TEST(test_slice_to_low_level_read_miss_reads_nand_and_issues_tx_dma);
	RUN_TEST(test_slice_to_low_level_read_of_unmapped_lsa_skips_nand);
	RUN_TEST(test_slice_to_low_level_full_slice_write_marks_dirty_and_issues_rx_dma);
	RUN_TEST(test_slice_to_low_level_partial_write_of_mapped_slice_triggers_read_modify_write);
	RUN_TEST(test_slice_to_low_level_partial_write_of_unmapped_slice_has_no_nand_read);
	RUN_TEST(test_slice_to_low_level_hit_reuses_buffer_and_chains_behind_previous_request);
	RUN_TEST(test_slice_to_low_level_processes_every_queued_slice);
	RUN_TEST(test_slice_to_low_level_with_empty_queue_is_noop);
	RUN_TEST(test_slice_to_low_level_rejects_unknown_req_code);

	RUN_TEST(test_evict_clean_entry_does_nothing);
	RUN_TEST(test_evict_dirty_entry_queues_nand_write_and_cleans_entry);
	RUN_TEST(test_evict_dirty_entry_blocked_behind_pending_rx_dma);
	RUN_TEST(test_write_back_runs_end_to_end_through_nand_scheduler);

	RUN_TEST(test_data_read_from_nand_unmapped_lsa_does_nothing);
	RUN_TEST(test_data_read_from_nand_mapped_lsa_queues_read_request);
	RUN_TEST(test_data_read_from_nand_of_unprogrammed_page_is_row_addr_blocked);

	RUN_TEST(test_check_buf_dep_reports_pass_and_blocked);
	RUN_TEST(test_check_row_addr_dep_read_select_passes_below_permitted_page);
	RUN_TEST(test_check_row_addr_dep_read_select_blocks_and_counts);
	RUN_TEST(test_check_row_addr_dep_read_release_decrements_on_pass);
	RUN_TEST(test_check_row_addr_dep_read_rejects_unknown_option);
	RUN_TEST(test_check_row_addr_dep_write_passes_only_on_next_page);
	RUN_TEST(test_check_row_addr_dep_erase_passes_when_block_fully_programmed_and_unread);
	RUN_TEST(test_check_row_addr_dep_erase_select_blocks_and_sets_flag);
	RUN_TEST(test_check_row_addr_dep_erase_release_blocks_without_touching_flag);
	RUN_TEST(test_check_row_addr_dep_erase_rejects_unknown_option);
	RUN_TEST(test_check_row_addr_dep_rejects_physical_addressing);
	RUN_TEST(test_check_row_addr_dep_rejects_unknown_req_code);

	RUN_TEST(test_update_dep_table_buf_blocked_read_counts_blocked_read);
	RUN_TEST(test_update_dep_table_buf_blocked_erase_sets_flag);
	RUN_TEST(test_update_dep_table_buf_blocked_write_leaves_table_untouched);
	RUN_TEST(test_update_dep_table_rejects_physical_addressing);
	RUN_TEST(test_update_dep_table_sync_release_then_read_becomes_row_addr_blocked);
	RUN_TEST(test_update_dep_table_sync_release_then_read_goes_to_nand_queue);
	RUN_TEST(test_update_dep_table_sync_release_still_buf_blocked_counts_read);

	RUN_TEST(test_select_low_level_nand_read_pass_goes_to_nand_queue);
	RUN_TEST(test_select_low_level_nand_read_blocked_goes_to_row_addr_queue);
	RUN_TEST(test_select_low_level_nand_without_dependency_check_skips_table);
	RUN_TEST(test_select_low_level_physical_address_uses_ch_way_fields);
	RUN_TEST(test_select_low_level_rejects_bad_nand_addr_option);
	RUN_TEST(test_select_low_level_rejects_unknown_req_type);
	RUN_TEST(test_select_low_level_nvme_dma_is_issued_immediately);
	RUN_TEST(test_select_low_level_buf_blocked_nvme_dma_waits_in_buf_dep_queue);
	RUN_TEST(test_select_low_level_buf_blocked_nand_read_updates_dependency_table);
	RUN_TEST(test_select_low_level_buf_blocked_nand_without_check_skips_table);

	RUN_TEST(test_release_buf_dep_with_no_successor_clears_entry_tail);
	RUN_TEST(test_release_buf_dep_keeps_tail_owned_by_another_request);
	RUN_TEST(test_release_buf_dep_clears_temp_entry_tail);
	RUN_TEST(test_release_buf_dep_unblocks_successor_nvme_dma);
	RUN_TEST(test_release_buf_dep_unblocks_successor_nand_write);
	RUN_TEST(test_release_buf_dep_successor_nand_read_still_row_addr_blocked);
	RUN_TEST(test_release_buf_dep_successor_nand_read_passes_when_page_programmed);
	RUN_TEST(test_release_buf_dep_successor_without_dependency_check_goes_to_nand);
	RUN_TEST(test_release_buf_dep_successor_not_in_buf_dep_queue_is_only_unlinked);
	RUN_TEST(test_release_buf_dep_successor_with_physical_address_asserts);

	RUN_TEST(test_release_row_addr_dep_moves_released_reads_and_keeps_blocked_ones);
	RUN_TEST(test_release_row_addr_dep_on_empty_queue_is_noop);
	RUN_TEST(test_release_row_addr_dep_rejects_request_without_dependency_check);

	RUN_TEST(test_issue_rx_dma_programs_one_transfer_per_block);
	RUN_TEST(test_issue_tx_dma_records_tx_fifo_tail);
	RUN_TEST(test_issue_dma_with_zero_blocks_programs_nothing);
	RUN_TEST(test_issue_dma_rejects_non_dma_req_code);
	RUN_TEST(test_check_done_dma_with_empty_queue_is_noop);
	RUN_TEST(test_check_done_dma_retires_completed_rx_and_tx_requests);
	RUN_TEST(test_check_done_dma_keeps_requests_while_hardware_is_busy);
	RUN_TEST(test_check_done_dma_releases_buffer_chain_of_completed_request);

	return UNITY_END();
}
