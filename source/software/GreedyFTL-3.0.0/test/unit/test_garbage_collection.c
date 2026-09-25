/* Unit tests for garbage_collection.c. */
#include "unity.h"

#include "fw_test.h"
#include "memory_map.h"

#define DIE 0
#define OTHER_DIE (USER_DIES - 1)

void setUp(void)
{
	fw_test_reset();
	fw_test_init_ftl();
}

void tearDown(void) {}

/* ---- helpers ------------------------------------------------------------ */

static GC_VICTIM_LIST_ENTRY *bucket(unsigned int dieNo, unsigned int invalidSliceCnt)
{
	return &gcVictimMapPtr->gcVictimList[dieNo][invalidSliceCnt];
}

static VIRTUAL_BLOCK_ENTRY *blk(unsigned int dieNo, unsigned int blockNo)
{
	return &virtualBlockMapPtr->block[dieNo][blockNo];
}

static void assert_bucket_empty(unsigned int dieNo, unsigned int invalidSliceCnt)
{
	TEST_ASSERT_EQUAL_HEX16(BLOCK_NONE, bucket(dieNo, invalidSliceCnt)->headBlock);
	TEST_ASSERT_EQUAL_HEX16(BLOCK_NONE, bucket(dieNo, invalidSliceCnt)->tailBlock);
}

/* Number of request-pool slots holding a NAND request with the given code. */
static unsigned int count_nand_reqs(unsigned int reqCode)
{
	unsigned int i, n = 0;

	for (i = 0; i < AVAILABLE_OUNTSTANDING_REQ_COUNT; i++)
		if (reqPoolPtr->reqPool[i].reqQueueType != REQ_QUEUE_TYPE_FREE &&
		    reqPoolPtr->reqPool[i].reqType == REQ_TYPE_NAND &&
		    reqPoolPtr->reqPool[i].reqCode == reqCode)
			n++;
	return n;
}

/* Find the queued NAND request of the given code addressed at vsa. */
static SSD_REQ_FORMAT *find_nand_req(unsigned int reqCode, unsigned int vsa)
{
	unsigned int i;

	for (i = 0; i < AVAILABLE_OUNTSTANDING_REQ_COUNT; i++)
		if (reqPoolPtr->reqPool[i].reqQueueType != REQ_QUEUE_TYPE_FREE &&
		    reqPoolPtr->reqPool[i].reqType == REQ_TYPE_NAND &&
		    reqPoolPtr->reqPool[i].reqCode == reqCode &&
		    reqPoolPtr->reqPool[i].nandInfo.virtualSliceAddr == vsa)
			return &reqPoolPtr->reqPool[i];
	return NULL;
}

/*
 * Take a block off the free list of dieNo and fully program it. The first
 * validCnt pages hold live data for LSAs lsaBase.., the next staleCnt pages
 * hold data for LSAs that have since been rewritten elsewhere (the logical
 * map no longer points at them), the rest are unwritten (LSA_NONE).
 * The block is queued on the victim bucket matching its invalid count.
 */
static unsigned int make_victim(unsigned int dieNo, unsigned int validCnt,
				unsigned int staleCnt, unsigned int lsaBase)
{
	unsigned int blockNo = GetFromFbList(dieNo, GET_FREE_BLOCK_GC);
	unsigned int page, vsa, lsa;

	TEST_ASSERT_NOT_EQUAL(BLOCK_FAIL, blockNo);
	TEST_ASSERT_LESS_OR_EQUAL_UINT(USER_PAGES_PER_BLOCK, validCnt + staleCnt);

	for (page = 0; page < validCnt + staleCnt; page++) {
		vsa = Vorg2VsaTranslation(dieNo, blockNo, page);
		lsa = lsaBase + page;
		virtualSliceMapPtr->virtualSlice[vsa].logicalSliceAddr = lsa;
		if (page < validCnt)
			logicalSliceMapPtr->logicalSlice[lsa].virtualSliceAddr = vsa;
		else
			logicalSliceMapPtr->logicalSlice[lsa].virtualSliceAddr = VSA_NONE;
	}

	blk(dieNo, blockNo)->currentPage = USER_PAGES_PER_BLOCK;
	blk(dieNo, blockNo)->invalidSliceCnt = SLICES_PER_BLOCK - validCnt;
	PutToGcVictimList(dieNo, blockNo, blk(dieNo, blockNo)->invalidSliceCnt);
	return blockNo;
}

/* ---- InitGcVictimMap ---------------------------------------------------- */

static void test_init_clears_every_bucket_of_every_die(void)
{
	unsigned int dieNo, cnt;

	PutToGcVictimList(DIE, 3, 1);
	PutToGcVictimList(OTHER_DIE, 4, SLICES_PER_BLOCK);

	InitGcVictimMap();

	TEST_ASSERT_EQUAL_PTR(fw_ptr(GC_VICTIM_MAP_ADDR), gcVictimMapPtr);
	for (dieNo = 0; dieNo < USER_DIES; dieNo++)
		for (cnt = 0; cnt <= SLICES_PER_BLOCK; cnt++)
			assert_bucket_empty(dieNo, cnt);
}

/* ---- PutToGcVictimList / GetFromGcVictimList ---------------------------- */

static void test_put_single_block_sets_head_and_tail(void)
{
	PutToGcVictimList(DIE, 7, 3);

	TEST_ASSERT_EQUAL_UINT(7, bucket(DIE, 3)->headBlock);
	TEST_ASSERT_EQUAL_UINT(7, bucket(DIE, 3)->tailBlock);
	TEST_ASSERT_EQUAL_HEX16(BLOCK_NONE, blk(DIE, 7)->prevBlock);
	TEST_ASSERT_EQUAL_HEX16(BLOCK_NONE, blk(DIE, 7)->nextBlock);
	TEST_ASSERT_EQUAL_UINT(7, GetFromGcVictimList(DIE));
	assert_bucket_empty(DIE, 3);
}

static void test_put_appends_to_tail_and_links_neighbours(void)
{
	PutToGcVictimList(DIE, 7, 3);
	PutToGcVictimList(DIE, 9, 3);
	PutToGcVictimList(DIE, 11, 3);

	TEST_ASSERT_EQUAL_UINT(7, bucket(DIE, 3)->headBlock);
	TEST_ASSERT_EQUAL_UINT(11, bucket(DIE, 3)->tailBlock);
	TEST_ASSERT_EQUAL_UINT(9, blk(DIE, 7)->nextBlock);
	TEST_ASSERT_EQUAL_UINT(7, blk(DIE, 9)->prevBlock);
	TEST_ASSERT_EQUAL_UINT(11, blk(DIE, 9)->nextBlock);
	TEST_ASSERT_EQUAL_UINT(9, blk(DIE, 11)->prevBlock);
	TEST_ASSERT_EQUAL_HEX16(BLOCK_NONE, blk(DIE, 11)->nextBlock);
}

static void test_get_pops_same_bucket_in_fifo_order(void)
{
	PutToGcVictimList(DIE, 7, 3);
	PutToGcVictimList(DIE, 9, 3);
	PutToGcVictimList(DIE, 11, 3);

	TEST_ASSERT_EQUAL_UINT(7, GetFromGcVictimList(DIE));
	TEST_ASSERT_EQUAL_UINT(9, bucket(DIE, 3)->headBlock);
	TEST_ASSERT_EQUAL_HEX16(BLOCK_NONE, blk(DIE, 9)->prevBlock);
	TEST_ASSERT_EQUAL_UINT(11, bucket(DIE, 3)->tailBlock);

	TEST_ASSERT_EQUAL_UINT(9, GetFromGcVictimList(DIE));
	TEST_ASSERT_EQUAL_UINT(11, GetFromGcVictimList(DIE));
	assert_bucket_empty(DIE, 3);
}

static void test_get_prefers_bucket_with_most_invalid_slices(void)
{
	PutToGcVictimList(DIE, 5, 1);
	PutToGcVictimList(DIE, 6, SLICES_PER_BLOCK / 2);
	PutToGcVictimList(DIE, 8, SLICES_PER_BLOCK);
	PutToGcVictimList(DIE, 9, SLICES_PER_BLOCK);
	PutToGcVictimList(DIE, 7, SLICES_PER_BLOCK - 1);

	TEST_ASSERT_EQUAL_UINT(8, GetFromGcVictimList(DIE));
	TEST_ASSERT_EQUAL_UINT(9, GetFromGcVictimList(DIE));
	TEST_ASSERT_EQUAL_UINT(7, GetFromGcVictimList(DIE));
	TEST_ASSERT_EQUAL_UINT(6, GetFromGcVictimList(DIE));
	TEST_ASSERT_EQUAL_UINT(5, GetFromGcVictimList(DIE));
	FW_EXPECT_ASSERT(GetFromGcVictimList(DIE));
}

static void test_get_ignores_zero_invalid_bucket(void)
{
	/* Bucket 0 holds blocks with nothing to reclaim; it is never a victim. */
	PutToGcVictimList(DIE, 5, 0);

	TEST_ASSERT_EQUAL_UINT(5, bucket(DIE, 0)->headBlock);
	FW_EXPECT_ASSERT(GetFromGcVictimList(DIE));
	TEST_ASSERT_EQUAL_UINT(5, bucket(DIE, 0)->headBlock);
}

static void test_victim_lists_are_per_die(void)
{
	PutToGcVictimList(DIE, 7, 3);
	PutToGcVictimList(OTHER_DIE, 8, SLICES_PER_BLOCK);

	TEST_ASSERT_EQUAL_UINT(8, GetFromGcVictimList(OTHER_DIE));
	FW_EXPECT_ASSERT(GetFromGcVictimList(OTHER_DIE));
	TEST_ASSERT_EQUAL_UINT(7, GetFromGcVictimList(DIE));
}

static void test_empty_victim_list_asserts(void)
{
	FW_EXPECT_ASSERT(GetFromGcVictimList(DIE));
}

/* ---- SelectiveGetFromGcVictimList --------------------------------------- */

static void queue_three(unsigned int cnt)
{
	blk(DIE, 7)->invalidSliceCnt = cnt;
	blk(DIE, 9)->invalidSliceCnt = cnt;
	blk(DIE, 11)->invalidSliceCnt = cnt;
	PutToGcVictimList(DIE, 7, cnt);
	PutToGcVictimList(DIE, 9, cnt);
	PutToGcVictimList(DIE, 11, cnt);
}

static void test_selective_removes_head(void)
{
	queue_three(4);

	SelectiveGetFromGcVictimList(DIE, 7);

	TEST_ASSERT_EQUAL_UINT(9, bucket(DIE, 4)->headBlock);
	TEST_ASSERT_EQUAL_UINT(11, bucket(DIE, 4)->tailBlock);
	TEST_ASSERT_EQUAL_HEX16(BLOCK_NONE, blk(DIE, 9)->prevBlock);
	TEST_ASSERT_EQUAL_UINT(9, GetFromGcVictimList(DIE));
	TEST_ASSERT_EQUAL_UINT(11, GetFromGcVictimList(DIE));
}

static void test_selective_removes_middle(void)
{
	queue_three(4);

	SelectiveGetFromGcVictimList(DIE, 9);

	TEST_ASSERT_EQUAL_UINT(7, bucket(DIE, 4)->headBlock);
	TEST_ASSERT_EQUAL_UINT(11, bucket(DIE, 4)->tailBlock);
	TEST_ASSERT_EQUAL_UINT(11, blk(DIE, 7)->nextBlock);
	TEST_ASSERT_EQUAL_UINT(7, blk(DIE, 11)->prevBlock);
	TEST_ASSERT_EQUAL_UINT(7, GetFromGcVictimList(DIE));
	TEST_ASSERT_EQUAL_UINT(11, GetFromGcVictimList(DIE));
	FW_EXPECT_ASSERT(GetFromGcVictimList(DIE));
}

static void test_selective_removes_tail(void)
{
	queue_three(4);

	SelectiveGetFromGcVictimList(DIE, 11);

	TEST_ASSERT_EQUAL_UINT(7, bucket(DIE, 4)->headBlock);
	TEST_ASSERT_EQUAL_UINT(9, bucket(DIE, 4)->tailBlock);
	TEST_ASSERT_EQUAL_HEX16(BLOCK_NONE, blk(DIE, 9)->nextBlock);
	TEST_ASSERT_EQUAL_UINT(7, GetFromGcVictimList(DIE));
	TEST_ASSERT_EQUAL_UINT(9, GetFromGcVictimList(DIE));
	FW_EXPECT_ASSERT(GetFromGcVictimList(DIE));
}

static void test_selective_removes_only_element(void)
{
	blk(DIE, 7)->invalidSliceCnt = 2;
	PutToGcVictimList(DIE, 7, 2);

	SelectiveGetFromGcVictimList(DIE, 7);

	assert_bucket_empty(DIE, 2);
	FW_EXPECT_ASSERT(GetFromGcVictimList(DIE));
}

/* ---- Integration with InvalidateOldVsa (the production caller) ---------- */

static void test_overwrite_moves_block_to_next_bucket(void)
{
	/* Slice allocation round-robins over dies, so LSA 0 and LSA USER_DIES
	 * land in the same block of the same die. */
	unsigned int vsa = AddrTransWrite(0);
	unsigned int dieNo = Vsa2VdieTranslation(vsa);
	unsigned int blockNo = Vsa2VblockTranslation(vsa);
	unsigned int i;

	for (i = 1; i <= USER_DIES; i++)
		AddrTransWrite(i);
	TEST_ASSERT_EQUAL_UINT(dieNo, Vsa2VdieTranslation(AddrTransRead(USER_DIES)));
	TEST_ASSERT_EQUAL_UINT(0, blk(dieNo, blockNo)->invalidSliceCnt);

	AddrTransWrite(0);
	TEST_ASSERT_EQUAL_UINT(1, blk(dieNo, blockNo)->invalidSliceCnt);
	TEST_ASSERT_EQUAL_UINT(blockNo, bucket(dieNo, 1)->headBlock);

	AddrTransWrite(USER_DIES);
	TEST_ASSERT_EQUAL_UINT(2, blk(dieNo, blockNo)->invalidSliceCnt);
	assert_bucket_empty(dieNo, 1);
	TEST_ASSERT_EQUAL_UINT(blockNo, bucket(dieNo, 2)->headBlock);
	TEST_ASSERT_EQUAL_UINT(blockNo, GetFromGcVictimList(dieNo));
}

/* ---- GarbageCollection -------------------------------------------------- */

static void test_gc_fully_invalid_victim_only_erases(void)
{
	unsigned int freeBefore = virtualDieMapPtr->die[DIE].freeBlockCnt;
	unsigned int eraseBefore = count_nand_reqs(REQ_CODE_ERASE);
	unsigned int blockNo = make_victim(DIE, 0, USER_PAGES_PER_BLOCK, 1000);
	unsigned int eraseCntBefore = blk(DIE, blockNo)->eraseCnt;
	SSD_REQ_FORMAT *erase;
	unsigned int page;

	GarbageCollection(DIE);

	TEST_ASSERT_EQUAL_UINT(0, count_nand_reqs(REQ_CODE_READ));
	TEST_ASSERT_EQUAL_UINT(0, count_nand_reqs(REQ_CODE_WRITE));
	TEST_ASSERT_EQUAL_UINT(eraseBefore + 1, count_nand_reqs(REQ_CODE_ERASE));

	erase = find_nand_req(REQ_CODE_ERASE, Vorg2VsaTranslation(DIE, blockNo, 0));
	TEST_ASSERT_NOT_NULL(erase);
	TEST_ASSERT_EQUAL_UINT(USER_PAGES_PER_BLOCK, erase->nandInfo.programmedPageCnt);
	TEST_ASSERT_EQUAL_UINT(REQ_OPT_DATA_BUF_NONE, erase->reqOpt.dataBufFormat);

	TEST_ASSERT_EQUAL_UINT(1, blk(DIE, blockNo)->free);
	TEST_ASSERT_EQUAL_UINT(0, blk(DIE, blockNo)->currentPage);
	TEST_ASSERT_EQUAL_UINT(0, blk(DIE, blockNo)->invalidSliceCnt);
	TEST_ASSERT_EQUAL_UINT(eraseCntBefore + 1, blk(DIE, blockNo)->eraseCnt);
	TEST_ASSERT_EQUAL_UINT(freeBefore, virtualDieMapPtr->die[DIE].freeBlockCnt);
	TEST_ASSERT_EQUAL_UINT(blockNo, virtualDieMapPtr->die[DIE].tailFreeBlock);
	for (page = 0; page < USER_PAGES_PER_BLOCK; page++)
		TEST_ASSERT_EQUAL_HEX32(LSA_NONE,
			virtualSliceMapPtr->virtualSlice[Vorg2VsaTranslation(DIE, blockNo, page)].logicalSliceAddr);
	assert_bucket_empty(DIE, SLICES_PER_BLOCK);
}

static void test_gc_victim_with_no_written_slices_only_erases(void)
{
	/* Counter says something is invalid, but every slice reads LSA_NONE:
	 * the copy loop walks all pages and copies nothing. */
	unsigned int freeBefore = virtualDieMapPtr->die[DIE].freeBlockCnt;
	unsigned int eraseBefore = count_nand_reqs(REQ_CODE_ERASE);
	unsigned int blockNo = GetFromFbList(DIE, GET_FREE_BLOCK_GC);

	blk(DIE, blockNo)->invalidSliceCnt = 1;
	PutToGcVictimList(DIE, blockNo, 1);

	GarbageCollection(DIE);

	TEST_ASSERT_EQUAL_UINT(0, count_nand_reqs(REQ_CODE_READ));
	TEST_ASSERT_EQUAL_UINT(0, count_nand_reqs(REQ_CODE_WRITE));
	TEST_ASSERT_EQUAL_UINT(eraseBefore + 1, count_nand_reqs(REQ_CODE_ERASE));
	TEST_ASSERT_EQUAL_UINT(1, blk(DIE, blockNo)->free);
	TEST_ASSERT_EQUAL_UINT(freeBefore, virtualDieMapPtr->die[DIE].freeBlockCnt);
}

static void test_gc_partially_valid_victim_copies_only_live_slices(void)
{
	const unsigned int validCnt = 3, staleCnt = 2, lsaBase = 2000;
	unsigned int eraseBefore = count_nand_reqs(REQ_CODE_ERASE);
	unsigned int blockNo = make_victim(DIE, validCnt, staleCnt, lsaBase);
	unsigned int currentBlock = virtualDieMapPtr->die[DIE].currentBlock;
	unsigned int pageBefore = blk(DIE, currentBlock)->currentPage;
	unsigned int page, lsa, oldVsa, newVsa;
	SSD_REQ_FORMAT *rd, *wr;

	GarbageCollection(DIE);

	TEST_ASSERT_EQUAL_UINT(validCnt, count_nand_reqs(REQ_CODE_READ));
	TEST_ASSERT_EQUAL_UINT(validCnt, count_nand_reqs(REQ_CODE_WRITE));
	TEST_ASSERT_EQUAL_UINT(eraseBefore + 1, count_nand_reqs(REQ_CODE_ERASE));

	for (page = 0; page < validCnt; page++) {
		lsa = lsaBase + page;
		oldVsa = Vorg2VsaTranslation(DIE, blockNo, page);
		newVsa = logicalSliceMapPtr->logicalSlice[lsa].virtualSliceAddr;

		/* remapped into the die's current block, in page order */
		TEST_ASSERT_EQUAL_HEX32(Vorg2VsaTranslation(DIE, currentBlock, pageBefore + page), newVsa);
		TEST_ASSERT_EQUAL_UINT(lsa, virtualSliceMapPtr->virtualSlice[newVsa].logicalSliceAddr);

		rd = find_nand_req(REQ_CODE_READ, oldVsa);
		TEST_ASSERT_NOT_NULL(rd);
		TEST_ASSERT_EQUAL_UINT(lsa, rd->logicalSliceAddr);
		TEST_ASSERT_EQUAL_UINT(REQ_OPT_DATA_BUF_TEMP_ENTRY, rd->reqOpt.dataBufFormat);
		TEST_ASSERT_EQUAL_UINT(REQ_OPT_NAND_ADDR_VSA, rd->reqOpt.nandAddr);
		TEST_ASSERT_EQUAL_UINT(REQ_OPT_NAND_ECC_ON, rd->reqOpt.nandEcc);
		TEST_ASSERT_EQUAL_UINT(REQ_OPT_NAND_ECC_WARNING_OFF, rd->reqOpt.nandEccWarning);
		TEST_ASSERT_EQUAL_UINT(REQ_OPT_ROW_ADDR_DEPENDENCY_CHECK, rd->reqOpt.rowAddrDependencyCheck);
		TEST_ASSERT_EQUAL_UINT(REQ_OPT_BLOCK_SPACE_MAIN, rd->reqOpt.blockSpace);
		TEST_ASSERT_EQUAL_UINT(DIE, rd->dataBufInfo.entry);

		wr = find_nand_req(REQ_CODE_WRITE, newVsa);
		TEST_ASSERT_NOT_NULL(wr);
		TEST_ASSERT_EQUAL_UINT(lsa, wr->logicalSliceAddr);
		TEST_ASSERT_EQUAL_UINT(REQ_OPT_DATA_BUF_TEMP_ENTRY, wr->reqOpt.dataBufFormat);
		TEST_ASSERT_EQUAL_UINT(DIE, wr->dataBufInfo.entry);
	}

	/* stale slices are dropped, not copied */
	for (page = validCnt; page < validCnt + staleCnt; page++) {
		TEST_ASSERT_NULL(find_nand_req(REQ_CODE_READ, Vorg2VsaTranslation(DIE, blockNo, page)));
		TEST_ASSERT_EQUAL_HEX32(VSA_NONE, logicalSliceMapPtr->logicalSlice[lsaBase + page].virtualSliceAddr);
	}

	TEST_ASSERT_EQUAL_UINT(pageBefore + validCnt, blk(DIE, currentBlock)->currentPage);
	TEST_ASSERT_EQUAL_UINT(1, blk(DIE, blockNo)->free);
	TEST_ASSERT_EQUAL_UINT(0, blk(DIE, blockNo)->invalidSliceCnt);
	TEST_ASSERT_EQUAL_UINT(blockNo, virtualDieMapPtr->die[DIE].tailFreeBlock);
}

static void test_gc_copy_requests_chain_on_temp_buffer(void)
{
	unsigned int blockNo = make_victim(DIE, 2, 0, 3000);
	unsigned int tail, prev;

	GarbageCollection(DIE);

	/* read0 -> write0 -> read1 -> write1 blocked on temp buffer DIE */
	tail = tempDataBufMapPtr->tempDataBuf[DIE].blockingReqTail;
	TEST_ASSERT_NOT_EQUAL(REQ_SLOT_TAG_NONE, tail);
	TEST_ASSERT_EQUAL_UINT(REQ_CODE_WRITE, reqPoolPtr->reqPool[tail].reqCode);
	prev = reqPoolPtr->reqPool[tail].prevBlockingReq;
	TEST_ASSERT_EQUAL_UINT(REQ_CODE_READ, reqPoolPtr->reqPool[prev].reqCode);
	TEST_ASSERT_EQUAL_UINT(tail, reqPoolPtr->reqPool[prev].nextBlockingReq);
	TEST_ASSERT_EQUAL_HEX32(Vorg2VsaTranslation(DIE, blockNo, 1),
				reqPoolPtr->reqPool[prev].nandInfo.virtualSliceAddr);
}

static void test_gc_victim_is_current_block_allocates_new_target(void)
{
	unsigned int currentBlock = virtualDieMapPtr->die[DIE].currentBlock;
	unsigned int vsa = Vorg2VsaTranslation(DIE, currentBlock, 0);
	unsigned int newBlock;

	/* One live slice in the write-frontier block, then declare it a victim. */
	virtualSliceMapPtr->virtualSlice[vsa].logicalSliceAddr = 42;
	logicalSliceMapPtr->logicalSlice[42].virtualSliceAddr = vsa;
	blk(DIE, currentBlock)->currentPage = 1;
	blk(DIE, currentBlock)->invalidSliceCnt = SLICES_PER_BLOCK - 1;
	PutToGcVictimList(DIE, currentBlock, SLICES_PER_BLOCK - 1);

	GarbageCollection(DIE);

	newBlock = virtualDieMapPtr->die[DIE].currentBlock;
	TEST_ASSERT_NOT_EQUAL(currentBlock, newBlock);
	TEST_ASSERT_EQUAL_HEX32(Vorg2VsaTranslation(DIE, newBlock, 0),
				logicalSliceMapPtr->logicalSlice[42].virtualSliceAddr);
	TEST_ASSERT_EQUAL_UINT(1, blk(DIE, newBlock)->currentPage);
	TEST_ASSERT_EQUAL_UINT(1, count_nand_reqs(REQ_CODE_READ));
	TEST_ASSERT_EQUAL_UINT(1, count_nand_reqs(REQ_CODE_WRITE));
	TEST_ASSERT_EQUAL_UINT(1, blk(DIE, currentBlock)->free);
}

static void test_gc_picks_most_invalid_block_first(void)
{
	unsigned int lessInvalid = make_victim(DIE, 4, 0, 4000);
	unsigned int moreInvalid = make_victim(DIE, 1, 0, 5000);

	GarbageCollection(DIE);

	TEST_ASSERT_EQUAL_UINT(1, blk(DIE, moreInvalid)->free);
	TEST_ASSERT_EQUAL_UINT(0, blk(DIE, lessInvalid)->free);
	TEST_ASSERT_EQUAL_UINT(1, count_nand_reqs(REQ_CODE_READ));
	TEST_ASSERT_EQUAL_UINT(lessInvalid, bucket(DIE, SLICES_PER_BLOCK - 4)->headBlock);
}

static void test_gc_on_die_without_victims_asserts(void)
{
	unsigned int eraseBefore = count_nand_reqs(REQ_CODE_ERASE);

	FW_EXPECT_ASSERT(GarbageCollection(DIE));

	TEST_ASSERT_EQUAL_UINT(eraseBefore, count_nand_reqs(REQ_CODE_ERASE));
}

static void test_gc_end_to_end_via_address_translation(void)
{
	/* Write through the public path, then rewrite the same LSAs so the
	 * first die's block accumulates two stale slices; GC must reclaim it
	 * while keeping the live copies reachable. */
	unsigned int vsa = AddrTransWrite(0);
	unsigned int dieNo = Vsa2VdieTranslation(vsa);
	unsigned int blockNo = Vsa2VblockTranslation(vsa);
	unsigned int round, i;

	for (round = 0; round < 2; round++)
		for (i = 0; i < USER_DIES; i++)
			AddrTransWrite(i);

	TEST_ASSERT_EQUAL_UINT(2, blk(dieNo, blockNo)->invalidSliceCnt);
	TEST_ASSERT_EQUAL_UINT(blockNo, bucket(dieNo, 2)->headBlock);

	GarbageCollection(dieNo);

	TEST_ASSERT_EQUAL_UINT(1, blk(dieNo, blockNo)->free);
	TEST_ASSERT_EQUAL_UINT(0, blk(dieNo, blockNo)->invalidSliceCnt);
	assert_bucket_empty(dieNo, 2);
	/* the live copy of LSA 0 survives in a fresh slice */
	TEST_ASSERT_NOT_EQUAL(VSA_FAIL, AddrTransRead(0));
	TEST_ASSERT_EQUAL_UINT(0,
		virtualSliceMapPtr->virtualSlice[AddrTransRead(0)].logicalSliceAddr);
}

int main(void)
{
	UNITY_BEGIN();
	RUN_TEST(test_init_clears_every_bucket_of_every_die);
	RUN_TEST(test_put_single_block_sets_head_and_tail);
	RUN_TEST(test_put_appends_to_tail_and_links_neighbours);
	RUN_TEST(test_get_pops_same_bucket_in_fifo_order);
	RUN_TEST(test_get_prefers_bucket_with_most_invalid_slices);
	RUN_TEST(test_get_ignores_zero_invalid_bucket);
	RUN_TEST(test_victim_lists_are_per_die);
	RUN_TEST(test_empty_victim_list_asserts);
	RUN_TEST(test_selective_removes_head);
	RUN_TEST(test_selective_removes_middle);
	RUN_TEST(test_selective_removes_tail);
	RUN_TEST(test_selective_removes_only_element);
	RUN_TEST(test_overwrite_moves_block_to_next_bucket);
	RUN_TEST(test_gc_fully_invalid_victim_only_erases);
	RUN_TEST(test_gc_victim_with_no_written_slices_only_erases);
	RUN_TEST(test_gc_partially_valid_victim_copies_only_live_slices);
	RUN_TEST(test_gc_copy_requests_chain_on_temp_buffer);
	RUN_TEST(test_gc_victim_is_current_block_allocates_new_target);
	RUN_TEST(test_gc_picks_most_invalid_block_first);
	RUN_TEST(test_gc_on_die_without_victims_asserts);
	RUN_TEST(test_gc_end_to_end_via_address_translation);
	return UNITY_END();
}
