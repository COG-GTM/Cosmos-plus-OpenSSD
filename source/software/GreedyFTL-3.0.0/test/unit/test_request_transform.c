/* Unit tests for request_transform.c: NVMe -> slice splitting, slice -> low
 * level (NAND / NVMe DMA) transformation, buffer and row-address dependency
 * tracking, blocked-request release and NVMe DMA issue/completion.
 *
 * The FTL is booted once in main() (a full InitFTL boot scans the whole NAND
 * array and takes ~1.5 s); setUp then re-initialises every table these tests
 * mutate -- request pool/queues, scheduler, dependency table, data buffers,
 * slice/block/die maps and the free-slice allocation cursor, host DMA state
 * and the IO mock log -- so tests stay independent. */
#include <string.h>
#include "unity.h"
#include "ftl_test_env.h"
#include "mock_io.h"
#include "request_transform.h"
#include "request_allocation.h"
#include "request_format.h"
#include "address_translation.h"
#include "data_buffer.h"
#include "memory_map.h"
#include "nvme/nvme.h"
#include "nvme/host_lld.h"
#include "request_schedule.h"

/* Non-static in request_transform.c but not exported by its header. */
unsigned int CheckBufDep(unsigned int reqSlotTag);
unsigned int CheckRowAddrDep(unsigned int reqSlotTag, unsigned int checkRowAddrDepOpt);
unsigned int UpdateRowAddrDepTableForBufBlockedReq(unsigned int reqSlotTag);

/* Non-static in address_translation.c but not exported by its header; used to
 * rewind free-block / free-page allocation state without re-scanning NAND. */
void InitDieMap(void);
void InitBlockMap(void);
void InitCurrentBlockOfDieMap(void);

/* Die 3 lands on channel 1 / way 1 with USER_CHANNELS = 2. */
#define TEST_DIE      3
#define TEST_CH       Vdie2PchTranslation(TEST_DIE)
#define TEST_WAY      Vdie2PwayTranslation(TEST_DIE)
#define TEST_BLOCK    5
#define TEST_PAGE     2
#define TEST_VSA      Vorg2VsaTranslation(TEST_DIE, TEST_BLOCK, TEST_PAGE)
#define TEST_LSA      42
#define TEST_CMD_SLOT 9

void setUp(void)
{
	InitReqPool();
	InitDependencyTable();
	InitReqScheduler();
	InitSliceMap();
	InitDieMap();
	InitBlockMap();
	InitCurrentBlockOfDieMap();
	sliceAllocationTargetDie = FindDieForFreeSliceAllocation();
	InitDataBuf();
	mock_io_reset();
	mock_io_set_read_handler(HOST_DMA_FIFO_CNT_REG_ADDR, ftl_test_dma_fifo_instant_done, NULL);
	memset(&g_hostDmaStatus, 0, sizeof(g_hostDmaStatus));
	memset(&g_hostDmaAssistStatus, 0, sizeof(g_hostDmaAssistStatus));
	ftl_test_assert_armed = 0;
	ftl_test_assert_hit = 0;
}
void tearDown(void) {}

/* ---------------------------------------------------------------- helpers */

static ROW_ADDR_DEPENDENCY_ENTRY *dep_entry(void)
{
	return &rowAddrDependencyTablePtr->block[TEST_CH][TEST_WAY][TEST_BLOCK];
}

static unsigned int total_nand_req_count(void)
{
	unsigned int ch, way, total = 0;
	for (ch = 0; ch < USER_CHANNELS; ch++)
		for (way = 0; way < USER_WAYS; way++)
			total += nandReqQ[ch][way].reqCnt;
	return total;
}

static unsigned int total_row_blocked_req_count(void)
{
	unsigned int ch, way, total = 0;
	for (ch = 0; ch < USER_CHANNELS; ch++)
		for (way = 0; way < USER_WAYS; way++)
			total += blockedByRowAddrDepReqQ[ch][way].reqCnt;
	return total;
}

/* Take a request from the free queue and fill it in as a NAND request that
 * addresses TEST_VSA (or the given vsa) with row-address checking enabled. */
static unsigned int make_nand_req(unsigned int reqCode, unsigned int vsa)
{
	unsigned int tag = GetFromFreeReqQ();
	SSD_REQ_FORMAT *req = &reqPoolPtr->reqPool[tag];

	req->reqType = REQ_TYPE_NAND;
	req->reqCode = reqCode;
	req->nvmeCmdSlotTag = TEST_CMD_SLOT;
	req->logicalSliceAddr = TEST_LSA;
	req->prevBlockingReq = REQ_SLOT_TAG_NONE;
	req->nextBlockingReq = REQ_SLOT_TAG_NONE;
	req->reqOpt.dataBufFormat = REQ_OPT_DATA_BUF_ENTRY;
	req->reqOpt.nandAddr = REQ_OPT_NAND_ADDR_VSA;
	req->reqOpt.nandEcc = REQ_OPT_NAND_ECC_ON;
	req->reqOpt.nandEccWarning = REQ_OPT_NAND_ECC_WARNING_ON;
	req->reqOpt.rowAddrDependencyCheck = REQ_OPT_ROW_ADDR_DEPENDENCY_CHECK;
	req->reqOpt.blockSpace = REQ_OPT_BLOCK_SPACE_MAIN;
	req->dataBufInfo.entry = 0;
	req->nandInfo.virtualSliceAddr = vsa;
	req->nandInfo.programmedPageCnt = 0;
	return tag;
}

/* NVMe DMA request on data buffer `entry` covering `numBlocks` 4 KiB blocks
 * starting at NVMe block `startIndex` of command TEST_CMD_SLOT. */
static unsigned int make_dma_req(unsigned int reqCode, unsigned int entry,
                                 unsigned int startIndex, unsigned int numBlocks)
{
	unsigned int tag = GetFromFreeReqQ();
	SSD_REQ_FORMAT *req = &reqPoolPtr->reqPool[tag];

	req->reqType = REQ_TYPE_NVME_DMA;
	req->reqCode = reqCode;
	req->nvmeCmdSlotTag = TEST_CMD_SLOT;
	req->logicalSliceAddr = TEST_LSA;
	req->prevBlockingReq = REQ_SLOT_TAG_NONE;
	req->nextBlockingReq = REQ_SLOT_TAG_NONE;
	req->reqOpt.dataBufFormat = REQ_OPT_DATA_BUF_ENTRY;
	req->dataBufInfo.entry = entry;
	req->nvmeDmaInfo.startIndex = startIndex;
	req->nvmeDmaInfo.nvmeBlockOffset = 0;
	req->nvmeDmaInfo.numOfNvmeBlock = numBlocks;
	return tag;
}

static void assert_slice_req(unsigned int tag, unsigned int reqCode, unsigned int lsa,
                             unsigned int startIndex, unsigned int blockOffset,
                             unsigned int numBlocks)
{
	SSD_REQ_FORMAT *req = &reqPoolPtr->reqPool[tag];

	TEST_ASSERT_EQUAL_UINT(REQ_TYPE_SLICE, req->reqType);
	TEST_ASSERT_EQUAL_UINT(reqCode, req->reqCode);
	TEST_ASSERT_EQUAL_UINT(TEST_CMD_SLOT, req->nvmeCmdSlotTag);
	TEST_ASSERT_EQUAL_UINT(lsa, req->logicalSliceAddr);
	TEST_ASSERT_EQUAL_UINT(startIndex, req->nvmeDmaInfo.startIndex);
	TEST_ASSERT_EQUAL_UINT(blockOffset, req->nvmeDmaInfo.nvmeBlockOffset);
	TEST_ASSERT_EQUAL_UINT(numBlocks, req->nvmeDmaInfo.numOfNvmeBlock);
	TEST_ASSERT_EQUAL_UINT(REQ_QUEUE_TYPE_SLICE, req->reqQueueType);
}

static unsigned int nth_slice_req(unsigned int n)
{
	unsigned int tag = sliceReqQ.headReq;
	while (n--)
		tag = reqPoolPtr->reqPool[tag].nextReq;
	return tag;
}

/* Expected fourth FIFO word written by set_auto_{rx,tx}_dma for a block. */
static unsigned int dma_cmd_word3(unsigned int direction, unsigned int cmd4KBOffset)
{
	HOST_DMA_CMD_FIFO_REG reg;
	reg.dword[3] = 0;
	reg.dmaType = HOST_DMA_AUTO_TYPE;
	reg.dmaDirection = direction;
	reg.cmd4KBOffset = cmd4KBOffset;
	reg.cmdSlotTag = TEST_CMD_SLOT;
	reg.autoCompletion = NVME_COMMAND_AUTO_COMPLETION_ON;
	return reg.dword[3];
}

/* Read handler that reports the DMA FIFO head as "nothing consumed yet". */
static uint32_t dma_fifo_nothing_done(uintptr_t addr, uint32_t stored, void *ctx)
{
	(void)addr; (void)stored; (void)ctx;
	return 0;
}

/* ------------------------------------------------- ReqTransNvmeToSlice */

static void test_read_spanning_two_slices_yields_two_slice_reqs(void)
{
	unsigned int first;

	TEST_ASSERT_EQUAL_UINT(0, sliceReqQ.reqCnt);
	ReqTransNvmeToSlice(3, NVME_BLOCKS_PER_SLICE - 1, 1, IO_NVM_READ);
	TEST_ASSERT_EQUAL_UINT(2, sliceReqQ.reqCnt);

	first = sliceReqQ.headReq;
	TEST_ASSERT_EQUAL_UINT(REQ_TYPE_SLICE, reqPoolPtr->reqPool[first].reqType);
	TEST_ASSERT_EQUAL_UINT(REQ_CODE_READ, reqPoolPtr->reqPool[first].reqCode);
	TEST_ASSERT_EQUAL_UINT(3, reqPoolPtr->reqPool[first].nvmeCmdSlotTag);
	TEST_ASSERT_EQUAL_UINT(0, reqPoolPtr->reqPool[first].logicalSliceAddr);
	TEST_ASSERT_EQUAL_UINT(1, reqPoolPtr->reqPool[sliceReqQ.tailReq].logicalSliceAddr);
}

static void test_aligned_full_slice_read_yields_one_full_slice_req(void)
{
	ReqTransNvmeToSlice(TEST_CMD_SLOT, 2 * NVME_BLOCKS_PER_SLICE, NVME_BLOCKS_PER_SLICE - 1, IO_NVM_READ);

	TEST_ASSERT_EQUAL_UINT(1, sliceReqQ.reqCnt);
	assert_slice_req(nth_slice_req(0), REQ_CODE_READ, 2, 0, 0, NVME_BLOCKS_PER_SLICE);
}

static void test_aligned_multi_slice_write_yields_only_full_slice_reqs(void)
{
	ReqTransNvmeToSlice(TEST_CMD_SLOT, 0, 3 * NVME_BLOCKS_PER_SLICE - 1, IO_NVM_WRITE);

	TEST_ASSERT_EQUAL_UINT(3, sliceReqQ.reqCnt);
	assert_slice_req(nth_slice_req(0), REQ_CODE_WRITE, 0, 0, 0, NVME_BLOCKS_PER_SLICE);
	assert_slice_req(nth_slice_req(1), REQ_CODE_WRITE, 1, NVME_BLOCKS_PER_SLICE, 0, NVME_BLOCKS_PER_SLICE);
	assert_slice_req(nth_slice_req(2), REQ_CODE_WRITE, 2, 2 * NVME_BLOCKS_PER_SLICE, 0, NVME_BLOCKS_PER_SLICE);
}

static void test_single_partial_slice_read_keeps_block_offset(void)
{
	/* LBA 1..2 inside slice 0: 2 blocks starting at block offset 1. */
	ReqTransNvmeToSlice(TEST_CMD_SLOT, 1, 1, IO_NVM_READ);

	TEST_ASSERT_EQUAL_UINT(1, sliceReqQ.reqCnt);
	assert_slice_req(nth_slice_req(0), REQ_CODE_READ, 0, 0, 1, 2);
}

static void test_single_partial_slice_write_keeps_block_offset(void)
{
	/* LBA 6 only: slice 1, block offset 2, one block. */
	ReqTransNvmeToSlice(TEST_CMD_SLOT, NVME_BLOCKS_PER_SLICE + 2, 0, IO_NVM_WRITE);

	TEST_ASSERT_EQUAL_UINT(1, sliceReqQ.reqCnt);
	assert_slice_req(nth_slice_req(0), REQ_CODE_WRITE, 1, 0, 2, 1);
}

static void test_unaligned_start_read_splits_into_partial_then_full_slice(void)
{
	/* LBA 2..7: 2 blocks of slice 0 then all of slice 1 (ends aligned). */
	ReqTransNvmeToSlice(TEST_CMD_SLOT, 2, 5, IO_NVM_READ);

	TEST_ASSERT_EQUAL_UINT(2, sliceReqQ.reqCnt);
	assert_slice_req(nth_slice_req(0), REQ_CODE_READ, 0, 0, 2, 2);
	assert_slice_req(nth_slice_req(1), REQ_CODE_READ, 1, 2, 0, NVME_BLOCKS_PER_SLICE);
}

static void test_unaligned_end_write_splits_into_full_then_partial_slice(void)
{
	/* LBA 0..5: all of slice 0 then first 2 blocks of slice 1. */
	ReqTransNvmeToSlice(TEST_CMD_SLOT, 0, 5, IO_NVM_WRITE);

	TEST_ASSERT_EQUAL_UINT(2, sliceReqQ.reqCnt);
	assert_slice_req(nth_slice_req(0), REQ_CODE_WRITE, 0, 0, 0, NVME_BLOCKS_PER_SLICE);
	assert_slice_req(nth_slice_req(1), REQ_CODE_WRITE, 1, NVME_BLOCKS_PER_SLICE, 0, 2);
}

static void test_unaligned_both_ends_read_spanning_three_slices(void)
{
	/* LBA 3..9: 1 block of slice 0, all of slice 1, 2 blocks of slice 2. */
	ReqTransNvmeToSlice(TEST_CMD_SLOT, 3, 6, IO_NVM_READ);

	TEST_ASSERT_EQUAL_UINT(3, sliceReqQ.reqCnt);
	assert_slice_req(nth_slice_req(0), REQ_CODE_READ, 0, 0, 3, 1);
	assert_slice_req(nth_slice_req(1), REQ_CODE_READ, 1, 1, 0, NVME_BLOCKS_PER_SLICE);
	assert_slice_req(nth_slice_req(2), REQ_CODE_READ, 2, 1 + NVME_BLOCKS_PER_SLICE, 0, 2);
}

static void test_unaligned_start_and_end_within_two_slices(void)
{
	/* LBA 3..4: last block of slice 0 and first block of slice 1. */
	ReqTransNvmeToSlice(TEST_CMD_SLOT, 3, 1, IO_NVM_WRITE);

	TEST_ASSERT_EQUAL_UINT(2, sliceReqQ.reqCnt);
	assert_slice_req(nth_slice_req(0), REQ_CODE_WRITE, 0, 0, 3, 1);
	assert_slice_req(nth_slice_req(1), REQ_CODE_WRITE, 1, 1, 0, 1);
}

static void test_nvme_to_slice_consumes_one_free_req_per_slice(void)
{
	unsigned int freeBefore = freeReqQ.reqCnt;

	ReqTransNvmeToSlice(TEST_CMD_SLOT, 3, 6, IO_NVM_READ);

	TEST_ASSERT_EQUAL_UINT(freeBefore - 3, freeReqQ.reqCnt);
}

static void test_unsupported_nvme_opcode_asserts(void)
{
	FTL_TEST_EXPECT_ASSERT(ReqTransNvmeToSlice(TEST_CMD_SLOT, 0, 0, IO_NVM_FLUSH));
}

/* ------------------------------------------- ReqTransSliceToLowLevel */

static void test_read_miss_of_unmapped_slice_issues_tx_dma_without_nand_read(void)
{
	unsigned int tag;

	ReqTransNvmeToSlice(TEST_CMD_SLOT, TEST_LSA * NVME_BLOCKS_PER_SLICE, NVME_BLOCKS_PER_SLICE - 1, IO_NVM_READ);
	tag = sliceReqQ.headReq;

	ReqTransSliceToLowLevel();

	TEST_ASSERT_EQUAL_UINT(0, sliceReqQ.reqCnt);
	TEST_ASSERT_EQUAL_UINT(0, total_nand_req_count());
	TEST_ASSERT_EQUAL_UINT(1, nvmeDmaReqQ.reqCnt);
	TEST_ASSERT_EQUAL_UINT(REQ_TYPE_NVME_DMA, reqPoolPtr->reqPool[tag].reqType);
	TEST_ASSERT_EQUAL_UINT(REQ_CODE_TxDMA, reqPoolPtr->reqPool[tag].reqCode);
	TEST_ASSERT_EQUAL_UINT(TEST_LSA, dataBufMapPtr->dataBuf[reqPoolPtr->reqPool[tag].dataBufInfo.entry].logicalSliceAddr);
	TEST_ASSERT_EQUAL_UINT(DATA_BUF_CLEAN, dataBufMapPtr->dataBuf[reqPoolPtr->reqPool[tag].dataBufInfo.entry].dirty);
	TEST_ASSERT_EQUAL_UINT(2 * NVME_BLOCKS_PER_SLICE, mock_io_write_count_for(HOST_DMA_CMD_FIFO_REG_ADDR)
	                       + mock_io_write_count_for(HOST_DMA_CMD_FIFO_REG_ADDR + 12));
}

static void test_read_miss_of_mapped_slice_issues_nand_read_and_blocks_tx_dma(void)
{
	unsigned int sliceTag, nandTag;

	logicalSliceMapPtr->logicalSlice[TEST_LSA].virtualSliceAddr = TEST_VSA;
	dep_entry()->permittedProgPage = TEST_PAGE + 1;

	ReqTransNvmeToSlice(TEST_CMD_SLOT, TEST_LSA * NVME_BLOCKS_PER_SLICE, NVME_BLOCKS_PER_SLICE - 1, IO_NVM_READ);
	sliceTag = sliceReqQ.headReq;

	ReqTransSliceToLowLevel();

	TEST_ASSERT_EQUAL_UINT(1, nandReqQ[TEST_CH][TEST_WAY].reqCnt);
	nandTag = nandReqQ[TEST_CH][TEST_WAY].headReq;
	TEST_ASSERT_EQUAL_UINT(REQ_TYPE_NAND, reqPoolPtr->reqPool[nandTag].reqType);
	TEST_ASSERT_EQUAL_UINT(REQ_CODE_READ, reqPoolPtr->reqPool[nandTag].reqCode);
	TEST_ASSERT_EQUAL_UINT(TEST_VSA, reqPoolPtr->reqPool[nandTag].nandInfo.virtualSliceAddr);
	TEST_ASSERT_EQUAL_UINT(TEST_LSA, reqPoolPtr->reqPool[nandTag].logicalSliceAddr);
	TEST_ASSERT_EQUAL_UINT(reqPoolPtr->reqPool[sliceTag].dataBufInfo.entry, reqPoolPtr->reqPool[nandTag].dataBufInfo.entry);

	/* The host transfer must wait for the NAND read on the same buffer. */
	TEST_ASSERT_EQUAL_UINT(0, nvmeDmaReqQ.reqCnt);
	TEST_ASSERT_EQUAL_UINT(1, blockedByBufDepReqQ.reqCnt);
	TEST_ASSERT_EQUAL_UINT(REQ_CODE_TxDMA, reqPoolPtr->reqPool[sliceTag].reqCode);
	TEST_ASSERT_EQUAL_UINT(nandTag, reqPoolPtr->reqPool[sliceTag].prevBlockingReq);
	TEST_ASSERT_EQUAL_UINT(sliceTag, reqPoolPtr->reqPool[nandTag].nextBlockingReq);
	TEST_ASSERT_EQUAL_UINT(0, mock_io_write_count_for(HOST_DMA_CMD_FIFO_REG_ADDR));
}

static void test_read_of_not_yet_programmed_page_is_blocked_by_row_addr_dep(void)
{
	logicalSliceMapPtr->logicalSlice[TEST_LSA].virtualSliceAddr = TEST_VSA;
	TEST_ASSERT_EQUAL_UINT(0, dep_entry()->permittedProgPage);

	ReqTransNvmeToSlice(TEST_CMD_SLOT, TEST_LSA * NVME_BLOCKS_PER_SLICE, NVME_BLOCKS_PER_SLICE - 1, IO_NVM_READ);
	ReqTransSliceToLowLevel();

	TEST_ASSERT_EQUAL_UINT(0, total_nand_req_count());
	TEST_ASSERT_EQUAL_UINT(1, blockedByRowAddrDepReqQ[TEST_CH][TEST_WAY].reqCnt);
	TEST_ASSERT_EQUAL_UINT(1, dep_entry()->blockedReadReqCnt);
	TEST_ASSERT_EQUAL_UINT(1, blockedByBufDepReqQ.reqCnt);
}

static void test_full_slice_write_marks_buffer_dirty_and_issues_rx_dma(void)
{
	unsigned int tag, entry;

	ReqTransNvmeToSlice(TEST_CMD_SLOT, TEST_LSA * NVME_BLOCKS_PER_SLICE, NVME_BLOCKS_PER_SLICE - 1, IO_NVM_WRITE);
	tag = sliceReqQ.headReq;

	ReqTransSliceToLowLevel();

	entry = reqPoolPtr->reqPool[tag].dataBufInfo.entry;
	TEST_ASSERT_EQUAL_UINT(0, total_nand_req_count());
	TEST_ASSERT_EQUAL_UINT(1, nvmeDmaReqQ.reqCnt);
	TEST_ASSERT_EQUAL_UINT(REQ_TYPE_NVME_DMA, reqPoolPtr->reqPool[tag].reqType);
	TEST_ASSERT_EQUAL_UINT(REQ_CODE_RxDMA, reqPoolPtr->reqPool[tag].reqCode);
	TEST_ASSERT_EQUAL_UINT(DATA_BUF_DIRTY, dataBufMapPtr->dataBuf[entry].dirty);
	TEST_ASSERT_EQUAL_UINT(TEST_LSA, dataBufMapPtr->dataBuf[entry].logicalSliceAddr);
	TEST_ASSERT_EQUAL_UINT(tag, dataBufMapPtr->dataBuf[entry].blockingReqTail);
}

static void test_partial_write_of_mapped_slice_reads_old_data_first(void)
{
	unsigned int nandTag;

	logicalSliceMapPtr->logicalSlice[TEST_LSA].virtualSliceAddr = TEST_VSA;
	dep_entry()->permittedProgPage = TEST_PAGE + 1;

	/* One block of the slice: read-modify-write. */
	ReqTransNvmeToSlice(TEST_CMD_SLOT, TEST_LSA * NVME_BLOCKS_PER_SLICE + 1, 0, IO_NVM_WRITE);
	ReqTransSliceToLowLevel();

	TEST_ASSERT_EQUAL_UINT(1, nandReqQ[TEST_CH][TEST_WAY].reqCnt);
	nandTag = nandReqQ[TEST_CH][TEST_WAY].headReq;
	TEST_ASSERT_EQUAL_UINT(REQ_CODE_READ, reqPoolPtr->reqPool[nandTag].reqCode);
	TEST_ASSERT_EQUAL_UINT(1, blockedByBufDepReqQ.reqCnt);
	TEST_ASSERT_EQUAL_UINT(REQ_CODE_RxDMA, reqPoolPtr->reqPool[blockedByBufDepReqQ.headReq].reqCode);
}

static void test_partial_write_of_unmapped_slice_needs_no_nand_read(void)
{
	ReqTransNvmeToSlice(TEST_CMD_SLOT, TEST_LSA * NVME_BLOCKS_PER_SLICE + 1, 0, IO_NVM_WRITE);
	ReqTransSliceToLowLevel();

	TEST_ASSERT_EQUAL_UINT(0, total_nand_req_count());
	TEST_ASSERT_EQUAL_UINT(1, nvmeDmaReqQ.reqCnt);
}

static void test_read_hit_reuses_buffer_and_waits_for_pending_write(void)
{
	unsigned int writeTag, readTag;

	ReqTransNvmeToSlice(TEST_CMD_SLOT, TEST_LSA * NVME_BLOCKS_PER_SLICE, NVME_BLOCKS_PER_SLICE - 1, IO_NVM_WRITE);
	writeTag = sliceReqQ.headReq;
	ReqTransSliceToLowLevel();

	ReqTransNvmeToSlice(TEST_CMD_SLOT, TEST_LSA * NVME_BLOCKS_PER_SLICE, NVME_BLOCKS_PER_SLICE - 1, IO_NVM_READ);
	readTag = sliceReqQ.headReq;
	ReqTransSliceToLowLevel();

	TEST_ASSERT_EQUAL_UINT(0, total_nand_req_count());
	TEST_ASSERT_EQUAL_UINT(reqPoolPtr->reqPool[writeTag].dataBufInfo.entry, reqPoolPtr->reqPool[readTag].dataBufInfo.entry);
	TEST_ASSERT_EQUAL_UINT(REQ_CODE_TxDMA, reqPoolPtr->reqPool[readTag].reqCode);
	TEST_ASSERT_EQUAL_UINT(writeTag, reqPoolPtr->reqPool[readTag].prevBlockingReq);
	TEST_ASSERT_EQUAL_UINT(1, nvmeDmaReqQ.reqCnt);
	TEST_ASSERT_EQUAL_UINT(1, blockedByBufDepReqQ.reqCnt);
	TEST_ASSERT_EQUAL_UINT(REQ_QUEUE_TYPE_BLOCKED_BY_BUF_DEP, reqPoolPtr->reqPool[readTag].reqQueueType);

	/* Write DMA completes -> the read is released and its Tx DMA issued. */
	CheckDoneNvmeDmaReq();
	TEST_ASSERT_EQUAL_UINT(1, nvmeDmaReqQ.reqCnt);
	TEST_ASSERT_EQUAL_UINT(readTag, nvmeDmaReqQ.headReq);
	TEST_ASSERT_EQUAL_UINT(0, blockedByBufDepReqQ.reqCnt);
	TEST_ASSERT_EQUAL_UINT(REQ_QUEUE_TYPE_FREE, reqPoolPtr->reqPool[writeTag].reqQueueType);
}

static void test_read_hit_on_clean_buffer_issues_tx_dma_immediately(void)
{
	unsigned int readTag;

	ReqTransNvmeToSlice(TEST_CMD_SLOT, TEST_LSA * NVME_BLOCKS_PER_SLICE, NVME_BLOCKS_PER_SLICE - 1, IO_NVM_READ);
	ReqTransSliceToLowLevel();
	CheckDoneNvmeDmaReq();
	TEST_ASSERT_EQUAL_UINT(0, nvmeDmaReqQ.reqCnt);

	ReqTransNvmeToSlice(TEST_CMD_SLOT, TEST_LSA * NVME_BLOCKS_PER_SLICE, 0, IO_NVM_READ);
	readTag = sliceReqQ.headReq;
	ReqTransSliceToLowLevel();

	TEST_ASSERT_EQUAL_UINT(0, total_nand_req_count());
	TEST_ASSERT_EQUAL_UINT(0, blockedByBufDepReqQ.reqCnt);
	TEST_ASSERT_EQUAL_UINT(1, nvmeDmaReqQ.reqCnt);
	TEST_ASSERT_EQUAL_UINT(readTag, nvmeDmaReqQ.headReq);
	TEST_ASSERT_EQUAL_UINT(REQ_CODE_TxDMA, reqPoolPtr->reqPool[readTag].reqCode);
}

static void test_evicting_dirty_lru_buffer_writes_it_to_nand(void)
{
	const unsigned int evictedLsa = 77;
	unsigned int victim, nandTag, vsa, dieNo;

	victim = dataBufLruList.tailEntry;
	dataBufMapPtr->dataBuf[victim].dirty = DATA_BUF_DIRTY;
	dataBufMapPtr->dataBuf[victim].logicalSliceAddr = evictedLsa;
	TEST_ASSERT_EQUAL_UINT(VSA_NONE, logicalSliceMapPtr->logicalSlice[evictedLsa].virtualSliceAddr);

	ReqTransNvmeToSlice(TEST_CMD_SLOT, TEST_LSA * NVME_BLOCKS_PER_SLICE, NVME_BLOCKS_PER_SLICE - 1, IO_NVM_READ);
	ReqTransSliceToLowLevel();

	vsa = logicalSliceMapPtr->logicalSlice[evictedLsa].virtualSliceAddr;
	TEST_ASSERT_NOT_EQUAL(VSA_NONE, vsa);
	dieNo = Vsa2VdieTranslation(vsa);

	TEST_ASSERT_EQUAL_UINT(1, total_nand_req_count());
	TEST_ASSERT_EQUAL_UINT(1, nandReqQ[Vdie2PchTranslation(dieNo)][Vdie2PwayTranslation(dieNo)].reqCnt);
	nandTag = nandReqQ[Vdie2PchTranslation(dieNo)][Vdie2PwayTranslation(dieNo)].headReq;
	TEST_ASSERT_EQUAL_UINT(REQ_CODE_WRITE, reqPoolPtr->reqPool[nandTag].reqCode);
	TEST_ASSERT_EQUAL_UINT(evictedLsa, reqPoolPtr->reqPool[nandTag].logicalSliceAddr);
	TEST_ASSERT_EQUAL_UINT(vsa, reqPoolPtr->reqPool[nandTag].nandInfo.virtualSliceAddr);
	TEST_ASSERT_EQUAL_UINT(victim, reqPoolPtr->reqPool[nandTag].dataBufInfo.entry);

	/* The buffer now belongs to the new read and is clean again. */
	TEST_ASSERT_EQUAL_UINT(DATA_BUF_CLEAN, dataBufMapPtr->dataBuf[victim].dirty);
	TEST_ASSERT_EQUAL_UINT(TEST_LSA, dataBufMapPtr->dataBuf[victim].logicalSliceAddr);
	/* The new read's host DMA queues behind the eviction write. */
	TEST_ASSERT_EQUAL_UINT(1, blockedByBufDepReqQ.reqCnt);
	TEST_ASSERT_EQUAL_UINT(nandTag, reqPoolPtr->reqPool[blockedByBufDepReqQ.headReq].prevBlockingReq);
}

static void test_evicting_clean_lru_buffer_issues_no_nand_write(void)
{
	unsigned int victim = dataBufLruList.tailEntry;

	dataBufMapPtr->dataBuf[victim].dirty = DATA_BUF_CLEAN;
	dataBufMapPtr->dataBuf[victim].logicalSliceAddr = 77;

	ReqTransNvmeToSlice(TEST_CMD_SLOT, TEST_LSA * NVME_BLOCKS_PER_SLICE, NVME_BLOCKS_PER_SLICE - 1, IO_NVM_READ);
	ReqTransSliceToLowLevel();

	TEST_ASSERT_EQUAL_UINT(0, total_nand_req_count());
	TEST_ASSERT_EQUAL_UINT(VSA_NONE, logicalSliceMapPtr->logicalSlice[77].virtualSliceAddr);
}

static void test_slice_to_low_level_with_empty_queue_is_a_no_op(void)
{
	unsigned int freeBefore = freeReqQ.reqCnt;

	ReqTransSliceToLowLevel();

	TEST_ASSERT_EQUAL_UINT(freeBefore, freeReqQ.reqCnt);
	TEST_ASSERT_EQUAL_UINT(0, nvmeDmaReqQ.reqCnt);
	TEST_ASSERT_EQUAL_UINT(0, mock_io_write_count_for(HOST_DMA_CMD_FIFO_REG_ADDR));
}

/* ------------------------------------------------------ CheckBufDep */

static void test_check_buf_dep_passes_without_blocking_predecessor(void)
{
	unsigned int tag = make_nand_req(REQ_CODE_READ, TEST_VSA);
	TEST_ASSERT_EQUAL_UINT(BUF_DEPENDENCY_REPORT_PASS, CheckBufDep(tag));
}

static void test_check_buf_dep_blocks_when_predecessor_present(void)
{
	unsigned int tag = make_nand_req(REQ_CODE_READ, TEST_VSA);
	reqPoolPtr->reqPool[tag].prevBlockingReq = 1;
	TEST_ASSERT_EQUAL_UINT(BUF_DEPENDENCY_REPORT_BLOCKED, CheckBufDep(tag));
}

/* -------------------------------------------------- CheckRowAddrDep */

static void test_row_dep_read_select_passes_for_programmed_page(void)
{
	unsigned int tag = make_nand_req(REQ_CODE_READ, TEST_VSA);
	dep_entry()->permittedProgPage = TEST_PAGE + 1;

	TEST_ASSERT_EQUAL_UINT(ROW_ADDR_DEPENDENCY_REPORT_PASS, CheckRowAddrDep(tag, ROW_ADDR_DEPENDENCY_CHECK_OPT_SELECT));
	TEST_ASSERT_EQUAL_UINT(0, dep_entry()->blockedReadReqCnt);
}

static void test_row_dep_read_select_blocks_and_counts_unprogrammed_page(void)
{
	unsigned int tag = make_nand_req(REQ_CODE_READ, TEST_VSA);
	dep_entry()->permittedProgPage = TEST_PAGE;

	TEST_ASSERT_EQUAL_UINT(ROW_ADDR_DEPENDENCY_REPORT_BLOCKED, CheckRowAddrDep(tag, ROW_ADDR_DEPENDENCY_CHECK_OPT_SELECT));
	TEST_ASSERT_EQUAL_UINT(1, dep_entry()->blockedReadReqCnt);
	TEST_ASSERT_EQUAL_UINT(ROW_ADDR_DEPENDENCY_REPORT_BLOCKED, CheckRowAddrDep(tag, ROW_ADDR_DEPENDENCY_CHECK_OPT_SELECT));
	TEST_ASSERT_EQUAL_UINT(2, dep_entry()->blockedReadReqCnt);
}

static void test_row_dep_read_release_passes_and_decrements_blocked_count(void)
{
	unsigned int tag = make_nand_req(REQ_CODE_READ, TEST_VSA);
	dep_entry()->permittedProgPage = TEST_PAGE + 1;
	dep_entry()->blockedReadReqCnt = 3;

	TEST_ASSERT_EQUAL_UINT(ROW_ADDR_DEPENDENCY_REPORT_PASS, CheckRowAddrDep(tag, ROW_ADDR_DEPENDENCY_CHECK_OPT_RELEASE));
	TEST_ASSERT_EQUAL_UINT(2, dep_entry()->blockedReadReqCnt);
}

static void test_row_dep_read_release_stays_blocked_without_touching_count(void)
{
	unsigned int tag = make_nand_req(REQ_CODE_READ, TEST_VSA);
	dep_entry()->permittedProgPage = TEST_PAGE;
	dep_entry()->blockedReadReqCnt = 3;

	TEST_ASSERT_EQUAL_UINT(ROW_ADDR_DEPENDENCY_REPORT_BLOCKED, CheckRowAddrDep(tag, ROW_ADDR_DEPENDENCY_CHECK_OPT_RELEASE));
	TEST_ASSERT_EQUAL_UINT(3, dep_entry()->blockedReadReqCnt);
}

static void test_row_dep_write_passes_only_for_next_page_in_order(void)
{
	unsigned int tag = make_nand_req(REQ_CODE_WRITE, TEST_VSA);

	dep_entry()->permittedProgPage = TEST_PAGE - 1;
	TEST_ASSERT_EQUAL_UINT(ROW_ADDR_DEPENDENCY_REPORT_BLOCKED, CheckRowAddrDep(tag, ROW_ADDR_DEPENDENCY_CHECK_OPT_SELECT));
	TEST_ASSERT_EQUAL_UINT(TEST_PAGE - 1, dep_entry()->permittedProgPage);

	dep_entry()->permittedProgPage = TEST_PAGE;
	TEST_ASSERT_EQUAL_UINT(ROW_ADDR_DEPENDENCY_REPORT_PASS, CheckRowAddrDep(tag, ROW_ADDR_DEPENDENCY_CHECK_OPT_SELECT));
	TEST_ASSERT_EQUAL_UINT(TEST_PAGE + 1, dep_entry()->permittedProgPage);
}

static void test_row_dep_erase_passes_when_block_fully_programmed_and_no_blocked_reads(void)
{
	unsigned int tag = make_nand_req(REQ_CODE_ERASE, TEST_VSA);
	reqPoolPtr->reqPool[tag].nandInfo.programmedPageCnt = 7;
	dep_entry()->permittedProgPage = 7;
	dep_entry()->blockedReadReqCnt = 0;
	dep_entry()->blockedEraseReqFlag = 1;

	TEST_ASSERT_EQUAL_UINT(ROW_ADDR_DEPENDENCY_REPORT_PASS, CheckRowAddrDep(tag, ROW_ADDR_DEPENDENCY_CHECK_OPT_RELEASE));
	TEST_ASSERT_EQUAL_UINT(0, dep_entry()->permittedProgPage);
	TEST_ASSERT_EQUAL_UINT(0, dep_entry()->blockedEraseReqFlag);
}

static void test_row_dep_erase_select_blocks_and_raises_flag_while_reads_pending(void)
{
	unsigned int tag = make_nand_req(REQ_CODE_ERASE, TEST_VSA);
	reqPoolPtr->reqPool[tag].nandInfo.programmedPageCnt = 7;
	dep_entry()->permittedProgPage = 7;
	dep_entry()->blockedReadReqCnt = 1;

	TEST_ASSERT_EQUAL_UINT(ROW_ADDR_DEPENDENCY_REPORT_BLOCKED, CheckRowAddrDep(tag, ROW_ADDR_DEPENDENCY_CHECK_OPT_SELECT));
	TEST_ASSERT_EQUAL_UINT(1, dep_entry()->blockedEraseReqFlag);
	TEST_ASSERT_EQUAL_UINT(7, dep_entry()->permittedProgPage);
}

static void test_row_dep_erase_release_blocks_without_raising_flag_when_pages_outstanding(void)
{
	unsigned int tag = make_nand_req(REQ_CODE_ERASE, TEST_VSA);
	reqPoolPtr->reqPool[tag].nandInfo.programmedPageCnt = 7;
	dep_entry()->permittedProgPage = 5;
	dep_entry()->blockedEraseReqFlag = 0;

	TEST_ASSERT_EQUAL_UINT(ROW_ADDR_DEPENDENCY_REPORT_BLOCKED, CheckRowAddrDep(tag, ROW_ADDR_DEPENDENCY_CHECK_OPT_RELEASE));
	TEST_ASSERT_EQUAL_UINT(0, dep_entry()->blockedEraseReqFlag);
}

static void test_row_dep_read_select_first_drains_pending_erase(void)
{
	unsigned int eraseTag = make_nand_req(REQ_CODE_ERASE, TEST_VSA);
	unsigned int readTag = make_nand_req(REQ_CODE_READ, TEST_VSA);

	/* An erase already blocked on this block, ready to be released. */
	reqPoolPtr->reqPool[eraseTag].nandInfo.programmedPageCnt = 0;
	dep_entry()->permittedProgPage = 0;
	dep_entry()->blockedEraseReqFlag = 1;
	PutToBlockedByRowAddrDepReqQ(eraseTag, TEST_CH, TEST_WAY);

	TEST_ASSERT_EQUAL_UINT(ROW_ADDR_DEPENDENCY_REPORT_BLOCKED, CheckRowAddrDep(readTag, ROW_ADDR_DEPENDENCY_CHECK_OPT_SELECT));

	TEST_ASSERT_EQUAL_UINT(0, dep_entry()->blockedEraseReqFlag);
	TEST_ASSERT_EQUAL_UINT(0, blockedByRowAddrDepReqQ[TEST_CH][TEST_WAY].reqCnt);
	TEST_ASSERT_EQUAL_UINT(1, dep_entry()->blockedReadReqCnt);
}

static void test_row_dep_asserts_on_unsupported_inputs(void)
{
	unsigned int tag = make_nand_req(REQ_CODE_READ, TEST_VSA);

	reqPoolPtr->reqPool[tag].reqOpt.nandAddr = REQ_OPT_NAND_ADDR_PHY_ORG;
	FTL_TEST_EXPECT_ASSERT(CheckRowAddrDep(tag, ROW_ADDR_DEPENDENCY_CHECK_OPT_SELECT));
	reqPoolPtr->reqPool[tag].reqOpt.nandAddr = REQ_OPT_NAND_ADDR_VSA;

	FTL_TEST_EXPECT_ASSERT(CheckRowAddrDep(tag, 2));

	reqPoolPtr->reqPool[tag].reqCode = REQ_CODE_ERASE;
	reqPoolPtr->reqPool[tag].nandInfo.programmedPageCnt = 5;
	FTL_TEST_EXPECT_ASSERT(CheckRowAddrDep(tag, 2));

	reqPoolPtr->reqPool[tag].reqCode = REQ_CODE_RxDMA;
	FTL_TEST_EXPECT_ASSERT(CheckRowAddrDep(tag, ROW_ADDR_DEPENDENCY_CHECK_OPT_SELECT));
}

/* ----------------------------- UpdateRowAddrDepTableForBufBlockedReq */

static void test_buf_blocked_read_registers_as_blocked_read(void)
{
	unsigned int tag = make_nand_req(REQ_CODE_READ, TEST_VSA);

	TEST_ASSERT_EQUAL_UINT(ROW_ADDR_DEPENDENCY_TABLE_UPDATE_REPORT_DONE, UpdateRowAddrDepTableForBufBlockedReq(tag));
	TEST_ASSERT_EQUAL_UINT(1, dep_entry()->blockedReadReqCnt);
}

static void test_buf_blocked_erase_raises_erase_flag(void)
{
	unsigned int tag = make_nand_req(REQ_CODE_ERASE, TEST_VSA);

	TEST_ASSERT_EQUAL_UINT(ROW_ADDR_DEPENDENCY_TABLE_UPDATE_REPORT_DONE, UpdateRowAddrDepTableForBufBlockedReq(tag));
	TEST_ASSERT_EQUAL_UINT(1, dep_entry()->blockedEraseReqFlag);
	TEST_ASSERT_EQUAL_UINT(0, dep_entry()->blockedReadReqCnt);
}

static void test_buf_blocked_write_leaves_table_untouched(void)
{
	unsigned int tag = make_nand_req(REQ_CODE_WRITE, TEST_VSA);

	TEST_ASSERT_EQUAL_UINT(ROW_ADDR_DEPENDENCY_TABLE_UPDATE_REPORT_DONE, UpdateRowAddrDepTableForBufBlockedReq(tag));
	TEST_ASSERT_EQUAL_UINT(0, dep_entry()->blockedReadReqCnt);
	TEST_ASSERT_EQUAL_UINT(0, dep_entry()->blockedEraseReqFlag);
}

static void test_buf_blocked_read_with_pending_erase_still_blocked_after_sync(void)
{
	unsigned int eraseTag = make_nand_req(REQ_CODE_ERASE, TEST_VSA);
	unsigned int readTag = make_nand_req(REQ_CODE_READ, TEST_VSA);

	dep_entry()->blockedEraseReqFlag = 1;
	PutToBlockedByRowAddrDepReqQ(eraseTag, TEST_CH, TEST_WAY);
	/* The read is still blocked by its buffer predecessor after the sync. */
	reqPoolPtr->reqPool[readTag].prevBlockingReq = eraseTag;

	TEST_ASSERT_EQUAL_UINT(ROW_ADDR_DEPENDENCY_TABLE_UPDATE_REPORT_DONE, UpdateRowAddrDepTableForBufBlockedReq(readTag));
	TEST_ASSERT_EQUAL_UINT(0, dep_entry()->blockedEraseReqFlag);
	TEST_ASSERT_EQUAL_UINT(1, dep_entry()->blockedReadReqCnt);
}

static void test_buf_blocked_read_freed_by_erase_sync_is_queued_directly(void)
{
	unsigned int eraseTag = make_nand_req(REQ_CODE_ERASE, TEST_VSA);
	unsigned int readTag = make_nand_req(REQ_CODE_READ, TEST_VSA);

	dep_entry()->blockedEraseReqFlag = 1;
	PutToBlockedByRowAddrDepReqQ(eraseTag, TEST_CH, TEST_WAY);

	/* Erase resets permittedProgPage to 0, so the read stays row-blocked. */
	TEST_ASSERT_EQUAL_UINT(ROW_ADDR_DEPENDENCY_TABLE_UPDATE_REPORT_SYNC, UpdateRowAddrDepTableForBufBlockedReq(readTag));
	TEST_ASSERT_EQUAL_UINT(0, dep_entry()->blockedEraseReqFlag);
	TEST_ASSERT_EQUAL_UINT(1, dep_entry()->blockedReadReqCnt);
	TEST_ASSERT_EQUAL_UINT(1, blockedByRowAddrDepReqQ[TEST_CH][TEST_WAY].reqCnt);
	TEST_ASSERT_EQUAL_UINT(readTag, blockedByRowAddrDepReqQ[TEST_CH][TEST_WAY].headReq);
}

static void test_buf_blocked_update_asserts_on_physical_address_format(void)
{
	unsigned int tag = make_nand_req(REQ_CODE_READ, TEST_VSA);
	reqPoolPtr->reqPool[tag].reqOpt.nandAddr = REQ_OPT_NAND_ADDR_PHY_ORG;

	FTL_TEST_EXPECT_ASSERT(UpdateRowAddrDepTableForBufBlockedReq(tag));
}

/* ------------------------------------------------ SelectLowLevelReqQ */

static void test_select_low_level_routes_nand_read_by_vsa_to_die_queue(void)
{
	unsigned int tag = make_nand_req(REQ_CODE_READ, TEST_VSA);
	dep_entry()->permittedProgPage = TEST_PAGE + 1;

	SelectLowLevelReqQ(tag);

	TEST_ASSERT_EQUAL_UINT(1, nandReqQ[TEST_CH][TEST_WAY].reqCnt);
	TEST_ASSERT_EQUAL_UINT(tag, nandReqQ[TEST_CH][TEST_WAY].headReq);
	TEST_ASSERT_EQUAL_UINT(REQ_QUEUE_TYPE_NAND, reqPoolPtr->reqPool[tag].reqQueueType);
}

static void test_select_low_level_routes_physical_address_without_dep_check(void)
{
	unsigned int tag = make_nand_req(REQ_CODE_ERASE, 0);
	reqPoolPtr->reqPool[tag].reqOpt.nandAddr = REQ_OPT_NAND_ADDR_PHY_ORG;
	reqPoolPtr->reqPool[tag].reqOpt.rowAddrDependencyCheck = REQ_OPT_ROW_ADDR_DEPENDENCY_NONE;
	reqPoolPtr->reqPool[tag].nandInfo.physicalCh = 1;
	reqPoolPtr->reqPool[tag].nandInfo.physicalWay = 6;
	reqPoolPtr->reqPool[tag].nandInfo.physicalBlock = 3;

	SelectLowLevelReqQ(tag);

	TEST_ASSERT_EQUAL_UINT(1, nandReqQ[1][6].reqCnt);
	TEST_ASSERT_EQUAL_UINT(tag, nandReqQ[1][6].headReq);
}

static void test_select_low_level_parks_row_blocked_read(void)
{
	unsigned int tag = make_nand_req(REQ_CODE_READ, TEST_VSA);

	SelectLowLevelReqQ(tag);

	TEST_ASSERT_EQUAL_UINT(0, total_nand_req_count());
	TEST_ASSERT_EQUAL_UINT(1, blockedByRowAddrDepReqQ[TEST_CH][TEST_WAY].reqCnt);
	TEST_ASSERT_EQUAL_UINT(REQ_QUEUE_TYPE_BLOCKED_BY_ROW_ADDR_DEP, reqPoolPtr->reqPool[tag].reqQueueType);
}

static void test_select_low_level_parks_buf_blocked_nand_req_and_updates_table(void)
{
	unsigned int tag = make_nand_req(REQ_CODE_READ, TEST_VSA);
	reqPoolPtr->reqPool[tag].prevBlockingReq = 0;

	SelectLowLevelReqQ(tag);

	TEST_ASSERT_EQUAL_UINT(1, blockedByBufDepReqQ.reqCnt);
	TEST_ASSERT_EQUAL_UINT(tag, blockedByBufDepReqQ.headReq);
	TEST_ASSERT_EQUAL_UINT(1, dep_entry()->blockedReadReqCnt);
}

static void test_select_low_level_parks_buf_blocked_dma_req_without_touching_table(void)
{
	unsigned int tag = make_dma_req(REQ_CODE_TxDMA, 0, 0, 1);
	reqPoolPtr->reqPool[tag].prevBlockingReq = 0;

	SelectLowLevelReqQ(tag);

	TEST_ASSERT_EQUAL_UINT(1, blockedByBufDepReqQ.reqCnt);
	TEST_ASSERT_EQUAL_UINT(0, nvmeDmaReqQ.reqCnt);
	TEST_ASSERT_EQUAL_UINT(0, mock_io_write_count_for(HOST_DMA_CMD_FIFO_REG_ADDR));
}

static void test_select_low_level_asserts_on_unsupported_type_and_options(void)
{
	unsigned int tag = make_nand_req(REQ_CODE_READ, TEST_VSA);

	reqPoolPtr->reqPool[tag].reqOpt.nandAddr = 3;
	FTL_TEST_EXPECT_ASSERT(SelectLowLevelReqQ(tag));
	reqPoolPtr->reqPool[tag].reqOpt.nandAddr = REQ_OPT_NAND_ADDR_VSA;

	reqPoolPtr->reqPool[tag].reqType = REQ_TYPE_SLICE;
	FTL_TEST_EXPECT_ASSERT(SelectLowLevelReqQ(tag));
}

/* ------------------------------------------- ReleaseBlockedByBufDepReq */

static void test_release_buf_dep_issues_waiting_dma_req(void)
{
	unsigned int first = make_dma_req(REQ_CODE_RxDMA, 3, 0, NVME_BLOCKS_PER_SLICE);
	unsigned int second = make_dma_req(REQ_CODE_TxDMA, 3, 0, NVME_BLOCKS_PER_SLICE);

	UpdateDataBufEntryInfoBlockingReq(3, first);
	UpdateDataBufEntryInfoBlockingReq(3, second);
	SelectLowLevelReqQ(second);
	TEST_ASSERT_EQUAL_UINT(1, blockedByBufDepReqQ.reqCnt);
	TEST_ASSERT_EQUAL_UINT(0, mock_io_write_count_for(HOST_DMA_CMD_FIFO_REG_ADDR));

	ReleaseBlockedByBufDepReq(first);

	TEST_ASSERT_EQUAL_UINT(0, blockedByBufDepReqQ.reqCnt);
	TEST_ASSERT_EQUAL_UINT(1, nvmeDmaReqQ.reqCnt);
	TEST_ASSERT_EQUAL_UINT(second, nvmeDmaReqQ.headReq);
	TEST_ASSERT_EQUAL_UINT(REQ_SLOT_TAG_NONE, reqPoolPtr->reqPool[second].prevBlockingReq);
	TEST_ASSERT_EQUAL_UINT(REQ_SLOT_TAG_NONE, reqPoolPtr->reqPool[first].nextBlockingReq);
	TEST_ASSERT_EQUAL_UINT(NVME_BLOCKS_PER_SLICE, mock_io_write_count_for(HOST_DMA_CMD_FIFO_REG_ADDR));
	/* `second` is now the last request on the buffer, so the tail stays. */
	TEST_ASSERT_EQUAL_UINT(second, dataBufMapPtr->dataBuf[3].blockingReqTail);
}

static void test_release_buf_dep_clears_buffer_tail_when_last_user_completes(void)
{
	unsigned int only = make_dma_req(REQ_CODE_RxDMA, 4, 0, 1);

	UpdateDataBufEntryInfoBlockingReq(4, only);
	TEST_ASSERT_EQUAL_UINT(only, dataBufMapPtr->dataBuf[4].blockingReqTail);

	ReleaseBlockedByBufDepReq(only);

	TEST_ASSERT_EQUAL_UINT(REQ_SLOT_TAG_NONE, dataBufMapPtr->dataBuf[4].blockingReqTail);
}

static void test_release_buf_dep_clears_temp_buffer_tail(void)
{
	unsigned int only = make_nand_req(REQ_CODE_READ, TEST_VSA);
	reqPoolPtr->reqPool[only].reqOpt.dataBufFormat = REQ_OPT_DATA_BUF_TEMP_ENTRY;
	reqPoolPtr->reqPool[only].dataBufInfo.entry = 2;
	tempDataBufMapPtr->tempDataBuf[2].blockingReqTail = only;

	ReleaseBlockedByBufDepReq(only);

	TEST_ASSERT_EQUAL_UINT(REQ_SLOT_TAG_NONE, tempDataBufMapPtr->tempDataBuf[2].blockingReqTail);
}

static void test_release_buf_dep_moves_nand_read_to_die_queue_when_page_permitted(void)
{
	unsigned int first = make_nand_req(REQ_CODE_WRITE, TEST_VSA);
	unsigned int second = make_nand_req(REQ_CODE_READ, TEST_VSA);

	UpdateDataBufEntryInfoBlockingReq(0, first);
	UpdateDataBufEntryInfoBlockingReq(0, second);
	SelectLowLevelReqQ(second);
	TEST_ASSERT_EQUAL_UINT(1, blockedByBufDepReqQ.reqCnt);
	TEST_ASSERT_EQUAL_UINT(1, dep_entry()->blockedReadReqCnt);

	dep_entry()->permittedProgPage = TEST_PAGE + 1;
	ReleaseBlockedByBufDepReq(first);

	TEST_ASSERT_EQUAL_UINT(0, blockedByBufDepReqQ.reqCnt);
	TEST_ASSERT_EQUAL_UINT(1, nandReqQ[TEST_CH][TEST_WAY].reqCnt);
	TEST_ASSERT_EQUAL_UINT(second, nandReqQ[TEST_CH][TEST_WAY].headReq);
	TEST_ASSERT_EQUAL_UINT(0, dep_entry()->blockedReadReqCnt);
}

static void test_release_buf_dep_parks_nand_read_when_page_not_yet_programmed(void)
{
	unsigned int first = make_nand_req(REQ_CODE_WRITE, TEST_VSA);
	unsigned int second = make_nand_req(REQ_CODE_READ, TEST_VSA);

	UpdateDataBufEntryInfoBlockingReq(0, first);
	UpdateDataBufEntryInfoBlockingReq(0, second);
	SelectLowLevelReqQ(second);

	ReleaseBlockedByBufDepReq(first);

	TEST_ASSERT_EQUAL_UINT(0, blockedByBufDepReqQ.reqCnt);
	TEST_ASSERT_EQUAL_UINT(0, total_nand_req_count());
	TEST_ASSERT_EQUAL_UINT(1, blockedByRowAddrDepReqQ[TEST_CH][TEST_WAY].reqCnt);
	TEST_ASSERT_EQUAL_UINT(1, dep_entry()->blockedReadReqCnt);
}

static void test_release_buf_dep_moves_nand_req_without_dep_check_directly(void)
{
	unsigned int first = make_nand_req(REQ_CODE_WRITE, TEST_VSA);
	unsigned int second = make_nand_req(REQ_CODE_WRITE, TEST_VSA);
	reqPoolPtr->reqPool[second].reqOpt.rowAddrDependencyCheck = REQ_OPT_ROW_ADDR_DEPENDENCY_NONE;

	UpdateDataBufEntryInfoBlockingReq(0, first);
	UpdateDataBufEntryInfoBlockingReq(0, second);
	SelectLowLevelReqQ(second);

	ReleaseBlockedByBufDepReq(first);

	TEST_ASSERT_EQUAL_UINT(1, nandReqQ[TEST_CH][TEST_WAY].reqCnt);
	TEST_ASSERT_EQUAL_UINT(second, nandReqQ[TEST_CH][TEST_WAY].headReq);
}

static void test_release_buf_dep_ignores_successor_not_parked_in_buf_dep_queue(void)
{
	unsigned int first = make_dma_req(REQ_CODE_RxDMA, 5, 0, 1);
	unsigned int second = make_dma_req(REQ_CODE_TxDMA, 5, 0, 1);

	UpdateDataBufEntryInfoBlockingReq(5, first);
	UpdateDataBufEntryInfoBlockingReq(5, second);
	/* `second` was never routed through SelectLowLevelReqQ. */

	ReleaseBlockedByBufDepReq(first);

	TEST_ASSERT_EQUAL_UINT(REQ_SLOT_TAG_NONE, reqPoolPtr->reqPool[second].prevBlockingReq);
	TEST_ASSERT_EQUAL_UINT(0, nvmeDmaReqQ.reqCnt);
	TEST_ASSERT_EQUAL_UINT(0, mock_io_write_count_for(HOST_DMA_CMD_FIFO_REG_ADDR));
}

static void test_release_buf_dep_asserts_on_unsupported_successor_options(void)
{
	unsigned int first = make_nand_req(REQ_CODE_WRITE, TEST_VSA);
	unsigned int second = make_nand_req(REQ_CODE_READ, TEST_VSA);

	UpdateDataBufEntryInfoBlockingReq(0, first);
	UpdateDataBufEntryInfoBlockingReq(0, second);
	SelectLowLevelReqQ(second);

	reqPoolPtr->reqPool[second].reqOpt.nandAddr = REQ_OPT_NAND_ADDR_PHY_ORG;
	FTL_TEST_EXPECT_ASSERT(ReleaseBlockedByBufDepReq(first));
}

/* --------------------------------------- ReleaseBlockedByRowAddrDepReq */

static void test_release_row_dep_moves_only_reads_of_programmed_pages(void)
{
	unsigned int lowPage = make_nand_req(REQ_CODE_READ, Vorg2VsaTranslation(TEST_DIE, TEST_BLOCK, 1));
	unsigned int highPage = make_nand_req(REQ_CODE_READ, Vorg2VsaTranslation(TEST_DIE, TEST_BLOCK, 6));

	SelectLowLevelReqQ(lowPage);
	SelectLowLevelReqQ(highPage);
	TEST_ASSERT_EQUAL_UINT(2, blockedByRowAddrDepReqQ[TEST_CH][TEST_WAY].reqCnt);
	TEST_ASSERT_EQUAL_UINT(2, dep_entry()->blockedReadReqCnt);

	dep_entry()->permittedProgPage = 3;
	ReleaseBlockedByRowAddrDepReq(TEST_CH, TEST_WAY);

	TEST_ASSERT_EQUAL_UINT(1, nandReqQ[TEST_CH][TEST_WAY].reqCnt);
	TEST_ASSERT_EQUAL_UINT(lowPage, nandReqQ[TEST_CH][TEST_WAY].headReq);
	TEST_ASSERT_EQUAL_UINT(1, blockedByRowAddrDepReqQ[TEST_CH][TEST_WAY].reqCnt);
	TEST_ASSERT_EQUAL_UINT(highPage, blockedByRowAddrDepReqQ[TEST_CH][TEST_WAY].headReq);
	TEST_ASSERT_EQUAL_UINT(1, dep_entry()->blockedReadReqCnt);

	dep_entry()->permittedProgPage = 7;
	ReleaseBlockedByRowAddrDepReq(TEST_CH, TEST_WAY);

	TEST_ASSERT_EQUAL_UINT(2, nandReqQ[TEST_CH][TEST_WAY].reqCnt);
	TEST_ASSERT_EQUAL_UINT(0, blockedByRowAddrDepReqQ[TEST_CH][TEST_WAY].reqCnt);
	TEST_ASSERT_EQUAL_UINT(0, dep_entry()->blockedReadReqCnt);
}

static void test_release_row_dep_on_empty_queue_does_nothing(void)
{
	ReleaseBlockedByRowAddrDepReq(TEST_CH, TEST_WAY);

	TEST_ASSERT_EQUAL_UINT(0, total_nand_req_count());
	TEST_ASSERT_EQUAL_UINT(0, total_row_blocked_req_count());
}

static void test_release_row_dep_asserts_on_req_without_dep_check(void)
{
	unsigned int tag = make_nand_req(REQ_CODE_READ, TEST_VSA);

	PutToBlockedByRowAddrDepReqQ(tag, TEST_CH, TEST_WAY);
	reqPoolPtr->reqPool[tag].reqOpt.rowAddrDependencyCheck = REQ_OPT_ROW_ADDR_DEPENDENCY_NONE;

	FTL_TEST_EXPECT_ASSERT(ReleaseBlockedByRowAddrDepReq(TEST_CH, TEST_WAY));
}

/* --------------------------------------------------- IssueNvmeDmaReq */

static void test_issue_tx_dma_writes_one_fifo_command_per_block(void)
{
	const unsigned int entry = 7, startIndex = 5, numBlocks = 3;
	unsigned int tag = make_dma_req(REQ_CODE_TxDMA, entry, startIndex, numBlocks);
	unsigned int expectedAddr = DATA_BUFFER_BASE_ADDR + entry * BYTES_PER_DATA_REGION_OF_SLICE;
	size_t i, logged = 0;

	IssueNvmeDmaReq(tag);

	TEST_ASSERT_EQUAL_UINT(numBlocks, mock_io_write_count_for(HOST_DMA_CMD_FIFO_REG_ADDR));
	TEST_ASSERT_EQUAL_UINT(numBlocks, mock_io_write_count_for(HOST_DMA_CMD_FIFO_REG_ADDR + 12));

	for (i = 0; i < mock_io_write_count(); i++) {
		const mock_io_write_t *w = mock_io_write_at(i);
		if (w->addr == HOST_DMA_CMD_FIFO_REG_ADDR) {
			TEST_ASSERT_EQUAL_HEX32(expectedAddr + logged * BYTES_PER_NVME_BLOCK, w->value);
		} else if (w->addr == HOST_DMA_CMD_FIFO_REG_ADDR + 12) {
			TEST_ASSERT_EQUAL_HEX32(dma_cmd_word3(HOST_DMA_TX_DIRECTION, startIndex + logged), w->value);
			logged++;
		}
	}
	TEST_ASSERT_EQUAL_UINT(numBlocks, logged);

	TEST_ASSERT_EQUAL_UINT(numBlocks, g_hostDmaStatus.fifoTail.autoDmaTx);
	TEST_ASSERT_EQUAL_UINT(0, g_hostDmaStatus.fifoTail.autoDmaRx);
	TEST_ASSERT_EQUAL_UINT(numBlocks, reqPoolPtr->reqPool[tag].nvmeDmaInfo.reqTail);
	TEST_ASSERT_EQUAL_UINT(0, reqPoolPtr->reqPool[tag].nvmeDmaInfo.overFlowCnt);
}

static void test_issue_rx_dma_uses_rx_direction_and_rx_tail(void)
{
	const unsigned int entry = 1, startIndex = 2, numBlocks = 2;
	unsigned int tag = make_dma_req(REQ_CODE_RxDMA, entry, startIndex, numBlocks);
	int found;

	IssueNvmeDmaReq(tag);

	TEST_ASSERT_EQUAL_UINT(numBlocks, mock_io_write_count_for(HOST_DMA_CMD_FIFO_REG_ADDR + 12));
	TEST_ASSERT_EQUAL_HEX32(dma_cmd_word3(HOST_DMA_RX_DIRECTION, startIndex + numBlocks - 1),
	                        mock_io_last_write(HOST_DMA_CMD_FIFO_REG_ADDR + 12, &found));
	TEST_ASSERT_TRUE(found);
	TEST_ASSERT_EQUAL_HEX32(DATA_BUFFER_BASE_ADDR + entry * BYTES_PER_DATA_REGION_OF_SLICE + BYTES_PER_NVME_BLOCK,
	                        mock_io_last_write(HOST_DMA_CMD_FIFO_REG_ADDR, &found));

	TEST_ASSERT_EQUAL_UINT(numBlocks, g_hostDmaStatus.fifoTail.autoDmaRx);
	TEST_ASSERT_EQUAL_UINT(0, g_hostDmaStatus.fifoTail.autoDmaTx);
	TEST_ASSERT_EQUAL_UINT(numBlocks, reqPoolPtr->reqPool[tag].nvmeDmaInfo.reqTail);
}

static void test_issue_dma_offsets_device_address_by_nvme_block_offset(void)
{
	unsigned int tag = make_dma_req(REQ_CODE_TxDMA, 0, 0, 1);
	int found;

	reqPoolPtr->reqPool[tag].nvmeDmaInfo.nvmeBlockOffset = 3;
	IssueNvmeDmaReq(tag);

	TEST_ASSERT_EQUAL_HEX32(DATA_BUFFER_BASE_ADDR + 3 * BYTES_PER_NVME_BLOCK,
	                        mock_io_last_write(HOST_DMA_CMD_FIFO_REG_ADDR, &found));
}

static void test_issue_dma_records_overflow_count_when_tail_wraps(void)
{
	unsigned int tag = make_dma_req(REQ_CODE_TxDMA, 0, 0, 2);

	g_hostDmaStatus.fifoTail.autoDmaTx = 255;
	IssueNvmeDmaReq(tag);

	TEST_ASSERT_EQUAL_UINT(1, g_hostDmaStatus.fifoTail.autoDmaTx);
	TEST_ASSERT_EQUAL_UINT(1, g_hostDmaAssistStatus.autoDmaTxOverFlowCnt);
	TEST_ASSERT_EQUAL_UINT(1, reqPoolPtr->reqPool[tag].nvmeDmaInfo.reqTail);
	TEST_ASSERT_EQUAL_UINT(1, reqPoolPtr->reqPool[tag].nvmeDmaInfo.overFlowCnt);
}

static void test_issue_dma_asserts_on_non_dma_req_code(void)
{
	unsigned int tag = make_dma_req(REQ_CODE_READ, 0, 0, 1);
	FTL_TEST_EXPECT_ASSERT(IssueNvmeDmaReq(tag));
}

/* ------------------------------------------------ CheckDoneNvmeDmaReq */

static void test_check_done_retires_completed_rx_and_tx_reqs(void)
{
	unsigned int rx = make_dma_req(REQ_CODE_RxDMA, 0, 0, 2);
	unsigned int tx = make_dma_req(REQ_CODE_TxDMA, 1, 0, 3);
	unsigned int freeBefore = freeReqQ.reqCnt;

	SelectLowLevelReqQ(rx);
	SelectLowLevelReqQ(tx);
	TEST_ASSERT_EQUAL_UINT(2, nvmeDmaReqQ.reqCnt);

	CheckDoneNvmeDmaReq();

	TEST_ASSERT_EQUAL_UINT(0, nvmeDmaReqQ.reqCnt);
	TEST_ASSERT_EQUAL_UINT(REQ_SLOT_TAG_NONE, nvmeDmaReqQ.headReq);
	TEST_ASSERT_EQUAL_UINT(REQ_QUEUE_TYPE_FREE, reqPoolPtr->reqPool[rx].reqQueueType);
	TEST_ASSERT_EQUAL_UINT(REQ_QUEUE_TYPE_FREE, reqPoolPtr->reqPool[tx].reqQueueType);
	TEST_ASSERT_EQUAL_UINT(freeBefore + 2, freeReqQ.reqCnt);
}

static void test_check_done_keeps_reqs_while_hardware_has_consumed_nothing(void)
{
	unsigned int rx = make_dma_req(REQ_CODE_RxDMA, 0, 0, 1);
	unsigned int tx = make_dma_req(REQ_CODE_TxDMA, 1, 0, 1);

	mock_io_set_read_handler(HOST_DMA_FIFO_CNT_REG_ADDR, dma_fifo_nothing_done, NULL);
	SelectLowLevelReqQ(rx);
	SelectLowLevelReqQ(tx);

	CheckDoneNvmeDmaReq();

	TEST_ASSERT_EQUAL_UINT(2, nvmeDmaReqQ.reqCnt);
	TEST_ASSERT_EQUAL_UINT(REQ_QUEUE_TYPE_NVME_DMA, reqPoolPtr->reqPool[rx].reqQueueType);
	TEST_ASSERT_EQUAL_UINT(REQ_QUEUE_TYPE_NVME_DMA, reqPoolPtr->reqPool[tx].reqQueueType);
}

static void test_check_done_retires_only_the_direction_that_finished(void)
{
	unsigned int rx = make_dma_req(REQ_CODE_RxDMA, 0, 0, 1);
	unsigned int tx = make_dma_req(REQ_CODE_TxDMA, 1, 0, 1);
	HOST_DMA_FIFO_CNT_REG head;

	SelectLowLevelReqQ(rx);
	SelectLowLevelReqQ(tx);

	/* Hardware reports the Tx FIFO drained but the Rx FIFO untouched. */
	head.dword = 0;
	head.autoDmaTx = g_hostDmaStatus.fifoTail.autoDmaTx;
	mock_io_set_read_handler(HOST_DMA_FIFO_CNT_REG_ADDR, NULL, NULL);
	mock_io_set_reg(HOST_DMA_FIFO_CNT_REG_ADDR, head.dword);

	CheckDoneNvmeDmaReq();

	TEST_ASSERT_EQUAL_UINT(1, nvmeDmaReqQ.reqCnt);
	TEST_ASSERT_EQUAL_UINT(rx, nvmeDmaReqQ.headReq);
	TEST_ASSERT_EQUAL_UINT(REQ_QUEUE_TYPE_FREE, reqPoolPtr->reqPool[tx].reqQueueType);
}

static void test_check_done_on_empty_queue_is_a_no_op(void)
{
	size_t readsBefore = mock_io_read_count();

	CheckDoneNvmeDmaReq();

	TEST_ASSERT_EQUAL_UINT(0, nvmeDmaReqQ.reqCnt);
	TEST_ASSERT_EQUAL_UINT(readsBefore, mock_io_read_count());
}

int main(void)
{
	ftl_test_env_reset();
	ftl_test_env_init_ftl();

	UNITY_BEGIN();

	RUN_TEST(test_read_spanning_two_slices_yields_two_slice_reqs);
	RUN_TEST(test_aligned_full_slice_read_yields_one_full_slice_req);
	RUN_TEST(test_aligned_multi_slice_write_yields_only_full_slice_reqs);
	RUN_TEST(test_single_partial_slice_read_keeps_block_offset);
	RUN_TEST(test_single_partial_slice_write_keeps_block_offset);
	RUN_TEST(test_unaligned_start_read_splits_into_partial_then_full_slice);
	RUN_TEST(test_unaligned_end_write_splits_into_full_then_partial_slice);
	RUN_TEST(test_unaligned_both_ends_read_spanning_three_slices);
	RUN_TEST(test_unaligned_start_and_end_within_two_slices);
	RUN_TEST(test_nvme_to_slice_consumes_one_free_req_per_slice);
	RUN_TEST(test_unsupported_nvme_opcode_asserts);

	RUN_TEST(test_read_miss_of_unmapped_slice_issues_tx_dma_without_nand_read);
	RUN_TEST(test_read_miss_of_mapped_slice_issues_nand_read_and_blocks_tx_dma);
	RUN_TEST(test_read_of_not_yet_programmed_page_is_blocked_by_row_addr_dep);
	RUN_TEST(test_full_slice_write_marks_buffer_dirty_and_issues_rx_dma);
	RUN_TEST(test_partial_write_of_mapped_slice_reads_old_data_first);
	RUN_TEST(test_partial_write_of_unmapped_slice_needs_no_nand_read);
	RUN_TEST(test_read_hit_reuses_buffer_and_waits_for_pending_write);
	RUN_TEST(test_read_hit_on_clean_buffer_issues_tx_dma_immediately);
	RUN_TEST(test_evicting_dirty_lru_buffer_writes_it_to_nand);
	RUN_TEST(test_evicting_clean_lru_buffer_issues_no_nand_write);
	RUN_TEST(test_slice_to_low_level_with_empty_queue_is_a_no_op);

	RUN_TEST(test_check_buf_dep_passes_without_blocking_predecessor);
	RUN_TEST(test_check_buf_dep_blocks_when_predecessor_present);

	RUN_TEST(test_row_dep_read_select_passes_for_programmed_page);
	RUN_TEST(test_row_dep_read_select_blocks_and_counts_unprogrammed_page);
	RUN_TEST(test_row_dep_read_release_passes_and_decrements_blocked_count);
	RUN_TEST(test_row_dep_read_release_stays_blocked_without_touching_count);
	RUN_TEST(test_row_dep_write_passes_only_for_next_page_in_order);
	RUN_TEST(test_row_dep_erase_passes_when_block_fully_programmed_and_no_blocked_reads);
	RUN_TEST(test_row_dep_erase_select_blocks_and_raises_flag_while_reads_pending);
	RUN_TEST(test_row_dep_erase_release_blocks_without_raising_flag_when_pages_outstanding);
	RUN_TEST(test_row_dep_read_select_first_drains_pending_erase);
	RUN_TEST(test_row_dep_asserts_on_unsupported_inputs);

	RUN_TEST(test_buf_blocked_read_registers_as_blocked_read);
	RUN_TEST(test_buf_blocked_erase_raises_erase_flag);
	RUN_TEST(test_buf_blocked_write_leaves_table_untouched);
	RUN_TEST(test_buf_blocked_read_with_pending_erase_still_blocked_after_sync);
	RUN_TEST(test_buf_blocked_read_freed_by_erase_sync_is_queued_directly);
	RUN_TEST(test_buf_blocked_update_asserts_on_physical_address_format);

	RUN_TEST(test_select_low_level_routes_nand_read_by_vsa_to_die_queue);
	RUN_TEST(test_select_low_level_routes_physical_address_without_dep_check);
	RUN_TEST(test_select_low_level_parks_row_blocked_read);
	RUN_TEST(test_select_low_level_parks_buf_blocked_nand_req_and_updates_table);
	RUN_TEST(test_select_low_level_parks_buf_blocked_dma_req_without_touching_table);
	RUN_TEST(test_select_low_level_asserts_on_unsupported_type_and_options);

	RUN_TEST(test_release_buf_dep_issues_waiting_dma_req);
	RUN_TEST(test_release_buf_dep_clears_buffer_tail_when_last_user_completes);
	RUN_TEST(test_release_buf_dep_clears_temp_buffer_tail);
	RUN_TEST(test_release_buf_dep_moves_nand_read_to_die_queue_when_page_permitted);
	RUN_TEST(test_release_buf_dep_parks_nand_read_when_page_not_yet_programmed);
	RUN_TEST(test_release_buf_dep_moves_nand_req_without_dep_check_directly);
	RUN_TEST(test_release_buf_dep_ignores_successor_not_parked_in_buf_dep_queue);
	RUN_TEST(test_release_buf_dep_asserts_on_unsupported_successor_options);

	RUN_TEST(test_release_row_dep_moves_only_reads_of_programmed_pages);
	RUN_TEST(test_release_row_dep_on_empty_queue_does_nothing);
	RUN_TEST(test_release_row_dep_asserts_on_req_without_dep_check);

	RUN_TEST(test_issue_tx_dma_writes_one_fifo_command_per_block);
	RUN_TEST(test_issue_rx_dma_uses_rx_direction_and_rx_tail);
	RUN_TEST(test_issue_dma_offsets_device_address_by_nvme_block_offset);
	RUN_TEST(test_issue_dma_records_overflow_count_when_tail_wraps);
	RUN_TEST(test_issue_dma_asserts_on_non_dma_req_code);

	RUN_TEST(test_check_done_retires_completed_rx_and_tx_reqs);
	RUN_TEST(test_check_done_keeps_reqs_while_hardware_has_consumed_nothing);
	RUN_TEST(test_check_done_retires_only_the_direction_that_finished);
	RUN_TEST(test_check_done_on_empty_queue_is_a_no_op);

	return UNITY_END();
}
