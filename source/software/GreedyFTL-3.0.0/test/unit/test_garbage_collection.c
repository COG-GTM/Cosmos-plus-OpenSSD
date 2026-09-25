/* Unit tests for garbage_collection.c: victim list bookkeeping
 * (PutToGcVictimList / GetFromGcVictimList / SelectiveGetFromGcVictimList)
 * and the GarbageCollection() copy-then-erase flow against the mocked NAND. */
#include "unity.h"
#include "ftl_test_env.h"
#include "address_translation.h"
#include "garbage_collection.h"
#include "request_allocation.h"
#include "request_schedule.h"
#include "request_format.h"
#include "request_transform.h"

void setUp(void) { ftl_test_env_reset(); ftl_test_env_init_ftl(); }
void tearDown(void) {}

#define DIE 0

static GC_VICTIM_LIST_ENTRY *victim_list(unsigned int die, unsigned int cnt)
{
	return &gcVictimMapPtr->gcVictimList[die][cnt];
}

static VIRTUAL_BLOCK_ENTRY *vblock(unsigned int die, unsigned int block)
{
	return &virtualBlockMapPtr->block[die][block];
}

/* Take a block off the free list so the tests can fill it without touching the
 * die's current write block. */
static unsigned int take_free_block(unsigned int die)
{
	unsigned int block = GetFromFbList(die, GET_FREE_BLOCK_GC);
	TEST_ASSERT_NOT_EQUAL_UINT(BLOCK_FAIL, block);
	return block;
}

/* Model a fully programmed block: every page of `block` maps logical slices
 * lsaBase.. (including the row-address dependency state a real write sequence
 * leaves behind), then invalidate the first `invalidCnt` of them through the
 * real firmware path (InvalidateOldVsa -> Selective/PutToGcVictimList). */
static void fill_block(unsigned int die, unsigned int block, unsigned int lsaBase, unsigned int invalidCnt)
{
	unsigned int page, vsa;

	for (page = 0; page < USER_PAGES_PER_BLOCK; page++)
	{
		vsa = Vorg2VsaTranslation(die, block, page);
		virtualSliceMapPtr->virtualSlice[vsa].logicalSliceAddr = lsaBase + page;
		logicalSliceMapPtr->logicalSlice[lsaBase + page].virtualSliceAddr = vsa;
	}
	vblock(die, block)->currentPage = USER_PAGES_PER_BLOCK;
	rowAddrDependencyTablePtr->block[Vdie2PchTranslation(die)][Vdie2PwayTranslation(die)][block].permittedProgPage = USER_PAGES_PER_BLOCK;

	for (page = 0; page < invalidCnt; page++)
		InvalidateOldVsa(lsaBase + page);

	TEST_ASSERT_EQUAL_UINT(invalidCnt, vblock(die, block)->invalidSliceCnt);
}

static int block_is_in_free_list(unsigned int die, unsigned int block)
{
	unsigned int b = virtualDieMapPtr->die[die].headFreeBlock;
	while (b != BLOCK_NONE)
	{
		if (b == block)
			return 1;
		b = vblock(die, b)->nextBlock;
	}
	return 0;
}

/* ------------------------------------------------------------------------ */
/* PutToGcVictimList                                                          */
/* ------------------------------------------------------------------------ */

static void test_put_first_block_becomes_head_and_tail(void)
{
	PutToGcVictimList(DIE, 7, 3);

	TEST_ASSERT_EQUAL_UINT(7, victim_list(DIE, 3)->headBlock);
	TEST_ASSERT_EQUAL_UINT(7, victim_list(DIE, 3)->tailBlock);
	TEST_ASSERT_EQUAL_UINT(BLOCK_NONE, vblock(DIE, 7)->prevBlock);
	TEST_ASSERT_EQUAL_UINT(BLOCK_NONE, vblock(DIE, 7)->nextBlock);
}

static void test_put_appends_to_tail_and_links_neighbours(void)
{
	PutToGcVictimList(DIE, 7, 3);
	PutToGcVictimList(DIE, 8, 3);
	PutToGcVictimList(DIE, 9, 3);

	TEST_ASSERT_EQUAL_UINT(7, victim_list(DIE, 3)->headBlock);
	TEST_ASSERT_EQUAL_UINT(9, victim_list(DIE, 3)->tailBlock);
	TEST_ASSERT_EQUAL_UINT(8, vblock(DIE, 7)->nextBlock);
	TEST_ASSERT_EQUAL_UINT(7, vblock(DIE, 8)->prevBlock);
	TEST_ASSERT_EQUAL_UINT(9, vblock(DIE, 8)->nextBlock);
	TEST_ASSERT_EQUAL_UINT(8, vblock(DIE, 9)->prevBlock);
	TEST_ASSERT_EQUAL_UINT(BLOCK_NONE, vblock(DIE, 9)->nextBlock);
}

static void test_put_keeps_lists_separate_per_die_and_invalid_count(void)
{
	PutToGcVictimList(0, 5, 2);
	PutToGcVictimList(1, 6, 2);
	PutToGcVictimList(0, 7, 4);

	TEST_ASSERT_EQUAL_UINT(5, victim_list(0, 2)->headBlock);
	TEST_ASSERT_EQUAL_UINT(5, victim_list(0, 2)->tailBlock);
	TEST_ASSERT_EQUAL_UINT(6, victim_list(1, 2)->headBlock);
	TEST_ASSERT_EQUAL_UINT(7, victim_list(0, 4)->headBlock);
	TEST_ASSERT_EQUAL_UINT(BLOCK_NONE, victim_list(1, 4)->headBlock);
}

/* ------------------------------------------------------------------------ */
/* GetFromGcVictimList                                                        */
/* ------------------------------------------------------------------------ */

static void test_get_returns_most_invalid_block_first(void)
{
	unsigned int mostly_invalid = 10, partly_invalid = 11;

	PutToGcVictimList(DIE, partly_invalid, 2);
	PutToGcVictimList(DIE, mostly_invalid, SLICES_PER_BLOCK);

	TEST_ASSERT_EQUAL_UINT(mostly_invalid, GetFromGcVictimList(DIE));
	TEST_ASSERT_EQUAL_UINT(BLOCK_NONE, victim_list(DIE, SLICES_PER_BLOCK)->headBlock);
	TEST_ASSERT_EQUAL_UINT(BLOCK_NONE, victim_list(DIE, SLICES_PER_BLOCK)->tailBlock);
	TEST_ASSERT_EQUAL_UINT(partly_invalid, GetFromGcVictimList(DIE));
	TEST_ASSERT_EQUAL_UINT(BLOCK_NONE, victim_list(DIE, 2)->headBlock);
}

static void test_get_pops_head_and_promotes_next_block(void)
{
	PutToGcVictimList(DIE, 7, 3);
	PutToGcVictimList(DIE, 8, 3);
	PutToGcVictimList(DIE, 9, 3);

	TEST_ASSERT_EQUAL_UINT(7, GetFromGcVictimList(DIE));
	TEST_ASSERT_EQUAL_UINT(8, victim_list(DIE, 3)->headBlock);
	TEST_ASSERT_EQUAL_UINT(9, victim_list(DIE, 3)->tailBlock);
	TEST_ASSERT_EQUAL_UINT(BLOCK_NONE, vblock(DIE, 8)->prevBlock);

	TEST_ASSERT_EQUAL_UINT(8, GetFromGcVictimList(DIE));
	TEST_ASSERT_EQUAL_UINT(9, GetFromGcVictimList(DIE));
	TEST_ASSERT_EQUAL_UINT(BLOCK_NONE, victim_list(DIE, 3)->headBlock);
	TEST_ASSERT_EQUAL_UINT(BLOCK_NONE, victim_list(DIE, 3)->tailBlock);
}

static void test_get_ignores_zero_invalid_bucket_and_asserts_when_empty(void)
{
	/* A block with no invalid slices is never a GC candidate. */
	PutToGcVictimList(DIE, 7, 0);

	FTL_TEST_EXPECT_ASSERT(GetFromGcVictimList(DIE));
	TEST_ASSERT_EQUAL_UINT(7, victim_list(DIE, 0)->headBlock);
}

static void test_get_asserts_on_empty_victim_list(void)
{
	FTL_TEST_EXPECT_ASSERT(GetFromGcVictimList(DIE));
}

static void test_get_does_not_see_other_dies_victims(void)
{
	PutToGcVictimList(1, 7, 3);

	FTL_TEST_EXPECT_ASSERT(GetFromGcVictimList(0));
	TEST_ASSERT_EQUAL_UINT(7, GetFromGcVictimList(1));
}

/* ------------------------------------------------------------------------ */
/* SelectiveGetFromGcVictimList                                               */
/* ------------------------------------------------------------------------ */

static void link_three(unsigned int cnt)
{
	vblock(DIE, 7)->invalidSliceCnt = cnt;
	vblock(DIE, 8)->invalidSliceCnt = cnt;
	vblock(DIE, 9)->invalidSliceCnt = cnt;
	PutToGcVictimList(DIE, 7, cnt);
	PutToGcVictimList(DIE, 8, cnt);
	PutToGcVictimList(DIE, 9, cnt);
}

static void test_selective_get_removes_middle_block(void)
{
	link_three(3);

	SelectiveGetFromGcVictimList(DIE, 8);

	TEST_ASSERT_EQUAL_UINT(7, victim_list(DIE, 3)->headBlock);
	TEST_ASSERT_EQUAL_UINT(9, victim_list(DIE, 3)->tailBlock);
	TEST_ASSERT_EQUAL_UINT(9, vblock(DIE, 7)->nextBlock);
	TEST_ASSERT_EQUAL_UINT(7, vblock(DIE, 9)->prevBlock);
}

static void test_selective_get_removes_tail_block(void)
{
	link_three(3);

	SelectiveGetFromGcVictimList(DIE, 9);

	TEST_ASSERT_EQUAL_UINT(7, victim_list(DIE, 3)->headBlock);
	TEST_ASSERT_EQUAL_UINT(8, victim_list(DIE, 3)->tailBlock);
	TEST_ASSERT_EQUAL_UINT(BLOCK_NONE, vblock(DIE, 8)->nextBlock);
}

static void test_selective_get_removes_head_block(void)
{
	link_three(3);

	SelectiveGetFromGcVictimList(DIE, 7);

	TEST_ASSERT_EQUAL_UINT(8, victim_list(DIE, 3)->headBlock);
	TEST_ASSERT_EQUAL_UINT(9, victim_list(DIE, 3)->tailBlock);
	TEST_ASSERT_EQUAL_UINT(BLOCK_NONE, vblock(DIE, 8)->prevBlock);
}

static void test_selective_get_removes_only_block_and_empties_list(void)
{
	vblock(DIE, 7)->invalidSliceCnt = 5;
	PutToGcVictimList(DIE, 7, 5);

	SelectiveGetFromGcVictimList(DIE, 7);

	TEST_ASSERT_EQUAL_UINT(BLOCK_NONE, victim_list(DIE, 5)->headBlock);
	TEST_ASSERT_EQUAL_UINT(BLOCK_NONE, victim_list(DIE, 5)->tailBlock);
	FTL_TEST_EXPECT_ASSERT(GetFromGcVictimList(DIE));
}

static void test_invalidating_a_slice_moves_block_to_higher_bucket(void)
{
	unsigned int block = take_free_block(DIE);

	fill_block(DIE, block, 100, 1);
	TEST_ASSERT_EQUAL_UINT(block, victim_list(DIE, 1)->headBlock);

	InvalidateOldVsa(101);

	TEST_ASSERT_EQUAL_UINT(BLOCK_NONE, victim_list(DIE, 1)->headBlock);
	TEST_ASSERT_EQUAL_UINT(block, victim_list(DIE, 2)->headBlock);
	TEST_ASSERT_EQUAL_UINT(block, victim_list(DIE, 2)->tailBlock);
	TEST_ASSERT_EQUAL_UINT(2, vblock(DIE, block)->invalidSliceCnt);
}

/* ------------------------------------------------------------------------ */
/* GarbageCollection                                                          */
/* ------------------------------------------------------------------------ */

static void test_gc_on_empty_victim_list_asserts(void)
{
	FTL_TEST_EXPECT_ASSERT(GarbageCollection(DIE));
}

static void test_gc_fully_invalid_block_only_erases(void)
{
	unsigned int block = take_free_block(DIE);
	unsigned int freeBefore = virtualDieMapPtr->die[DIE].freeBlockCnt;
	unsigned int eraseCntBefore = vblock(DIE, block)->eraseCnt;
	size_t readsBefore = mock_nsc_count_op(MOCK_NSC_OP_READ_TRIGGER);
	size_t programsBefore = mock_nsc_count_op(MOCK_NSC_OP_PROGRAM);
	size_t erasesBefore = mock_nsc_count_op(MOCK_NSC_OP_ERASE);
	unsigned int page;

	fill_block(DIE, block, 200, USER_PAGES_PER_BLOCK);
	TEST_ASSERT_EQUAL_UINT(block, victim_list(DIE, SLICES_PER_BLOCK)->headBlock);

	GarbageCollection(DIE);
	SyncAllLowLevelReqDone();

	TEST_ASSERT_EQUAL_size_t(readsBefore, mock_nsc_count_op(MOCK_NSC_OP_READ_TRIGGER));
	TEST_ASSERT_EQUAL_size_t(programsBefore, mock_nsc_count_op(MOCK_NSC_OP_PROGRAM));
	TEST_ASSERT_EQUAL_size_t(erasesBefore + 1, mock_nsc_count_op(MOCK_NSC_OP_ERASE));

	TEST_ASSERT_EQUAL_UINT(BLOCK_NONE, victim_list(DIE, SLICES_PER_BLOCK)->headBlock);
	TEST_ASSERT_EQUAL_UINT(1, vblock(DIE, block)->free);
	TEST_ASSERT_EQUAL_UINT(0, vblock(DIE, block)->invalidSliceCnt);
	TEST_ASSERT_EQUAL_UINT(0, vblock(DIE, block)->currentPage);
	TEST_ASSERT_EQUAL_UINT(eraseCntBefore + 1, vblock(DIE, block)->eraseCnt);
	TEST_ASSERT_EQUAL_UINT(freeBefore + 1, virtualDieMapPtr->die[DIE].freeBlockCnt);
	TEST_ASSERT_TRUE(block_is_in_free_list(DIE, block));

	for (page = 0; page < USER_PAGES_PER_BLOCK; page++)
		TEST_ASSERT_EQUAL_UINT(LSA_NONE, virtualSliceMapPtr->virtualSlice[Vorg2VsaTranslation(DIE, block, page)].logicalSliceAddr);

	TEST_ASSERT_EQUAL_UINT(0, notCompletedNandReqCnt);
	TEST_ASSERT_EQUAL_UINT(0, blockedReqCnt);
}

static void test_gc_partially_valid_block_copies_valid_slices_then_erases(void)
{
	const unsigned int lsaBase = 300;
	const unsigned int invalidCnt = USER_PAGES_PER_BLOCK / 2;
	const unsigned int validCnt = USER_PAGES_PER_BLOCK - invalidCnt;
	unsigned int block = take_free_block(DIE);
	unsigned int currentBlock = virtualDieMapPtr->die[DIE].currentBlock;
	unsigned int freeReqBefore = freeReqQ.reqCnt;
	size_t readsBefore = mock_nsc_count_op(MOCK_NSC_OP_READ_TRIGGER);
	size_t programsBefore = mock_nsc_count_op(MOCK_NSC_OP_PROGRAM);
	size_t erasesBefore = mock_nsc_count_op(MOCK_NSC_OP_ERASE);
	unsigned int page, lsa, vsa;

	fill_block(DIE, block, lsaBase, invalidCnt);
	TEST_ASSERT_EQUAL_UINT(block, victim_list(DIE, invalidCnt)->headBlock);

	GarbageCollection(DIE);

	/* One read + one write request per valid slice, plus the erase. */
	TEST_ASSERT_EQUAL_UINT(2 * validCnt + 1, freeReqBefore - freeReqQ.reqCnt);

	SyncAllLowLevelReqDone();

	TEST_ASSERT_EQUAL_size_t(readsBefore + validCnt, mock_nsc_count_op(MOCK_NSC_OP_READ_TRIGGER));
	TEST_ASSERT_EQUAL_size_t(programsBefore + validCnt, mock_nsc_count_op(MOCK_NSC_OP_PROGRAM));
	TEST_ASSERT_EQUAL_size_t(erasesBefore + 1, mock_nsc_count_op(MOCK_NSC_OP_ERASE));

	/* Valid slices were relocated into the die's current write block, in order. */
	for (page = invalidCnt; page < USER_PAGES_PER_BLOCK; page++)
	{
		lsa = lsaBase + page;
		vsa = logicalSliceMapPtr->logicalSlice[lsa].virtualSliceAddr;
		TEST_ASSERT_NOT_EQUAL_UINT(VSA_NONE, vsa);
		TEST_ASSERT_EQUAL_UINT(DIE, Vsa2VdieTranslation(vsa));
		TEST_ASSERT_EQUAL_UINT(currentBlock, Vsa2VblockTranslation(vsa));
		TEST_ASSERT_EQUAL_UINT(page - invalidCnt, Vsa2VpageTranslation(vsa));
		TEST_ASSERT_EQUAL_UINT(lsa, virtualSliceMapPtr->virtualSlice[vsa].logicalSliceAddr);
	}
	TEST_ASSERT_EQUAL_UINT(validCnt, vblock(DIE, currentBlock)->currentPage);

	/* Invalidated slices stay unmapped. */
	for (page = 0; page < invalidCnt; page++)
		TEST_ASSERT_EQUAL_UINT(VSA_NONE, logicalSliceMapPtr->logicalSlice[lsaBase + page].virtualSliceAddr);

	/* Victim block is erased and back on the free list. */
	TEST_ASSERT_EQUAL_UINT(1, vblock(DIE, block)->free);
	TEST_ASSERT_EQUAL_UINT(0, vblock(DIE, block)->invalidSliceCnt);
	TEST_ASSERT_TRUE(block_is_in_free_list(DIE, block));
	TEST_ASSERT_EQUAL_UINT(BLOCK_NONE, victim_list(DIE, invalidCnt)->headBlock);

	/* All requests drained back to the free pool. */
	TEST_ASSERT_EQUAL_UINT(0, notCompletedNandReqCnt);
	TEST_ASSERT_EQUAL_UINT(0, blockedReqCnt);
	TEST_ASSERT_EQUAL_UINT(freeReqBefore, freeReqQ.reqCnt);
}

static void test_gc_copy_requests_use_temp_buffers_and_vsa_addressing(void)
{
	const unsigned int lsaBase = 400;
	unsigned int block = take_free_block(DIE);
	unsigned int tag, readTag = REQ_SLOT_TAG_NONE, writeTag = REQ_SLOT_TAG_NONE, eraseTag = REQ_SLOT_TAG_NONE;
	unsigned int chNo = Vdie2PchTranslation(DIE), wayNo = Vdie2PwayTranslation(DIE);
	unsigned int nandReqCnt = 0;

	/* Exactly one valid slice: the last page. */
	fill_block(DIE, block, lsaBase, USER_PAGES_PER_BLOCK - 1);

	/* Stall the NAND so the requests stay queued for inspection. */
	mock_nsc_set_ready_busy(chCtlReg[chNo], 0);

	GarbageCollection(DIE);

	/* Read and erase go straight to the NAND queue; the write shares the read's
	 * temp buffer entry and waits on the buffer-dependency queue. */
	for (tag = nandReqQ[chNo][wayNo].headReq; tag != REQ_SLOT_TAG_NONE; tag = reqPoolPtr->reqPool[tag].nextReq)
	{
		nandReqCnt++;
		TEST_ASSERT_EQUAL_UINT(REQ_TYPE_NAND, reqPoolPtr->reqPool[tag].reqType);
		TEST_ASSERT_EQUAL_UINT(REQ_OPT_NAND_ADDR_VSA, reqPoolPtr->reqPool[tag].reqOpt.nandAddr);
		TEST_ASSERT_EQUAL_UINT(REQ_OPT_BLOCK_SPACE_MAIN, reqPoolPtr->reqPool[tag].reqOpt.blockSpace);
		switch (reqPoolPtr->reqPool[tag].reqCode)
		{
		case REQ_CODE_READ:  readTag = tag; break;
		case REQ_CODE_ERASE: eraseTag = tag; break;
		default: TEST_FAIL_MESSAGE("unexpected request code on NAND queue");
		}
	}
	TEST_ASSERT_EQUAL_UINT(2, nandReqCnt);
	TEST_ASSERT_NOT_EQUAL_UINT(REQ_SLOT_TAG_NONE, readTag);

	TEST_ASSERT_EQUAL_UINT(1, blockedByBufDepReqQ.reqCnt);
	writeTag = blockedByBufDepReqQ.headReq;
	TEST_ASSERT_EQUAL_UINT(REQ_TYPE_NAND, reqPoolPtr->reqPool[writeTag].reqType);
	TEST_ASSERT_EQUAL_UINT(REQ_CODE_WRITE, reqPoolPtr->reqPool[writeTag].reqCode);
	TEST_ASSERT_EQUAL_UINT(REQ_OPT_NAND_ADDR_VSA, reqPoolPtr->reqPool[writeTag].reqOpt.nandAddr);
	TEST_ASSERT_EQUAL_UINT(REQ_OPT_BLOCK_SPACE_MAIN, reqPoolPtr->reqPool[writeTag].reqOpt.blockSpace);
	TEST_ASSERT_NOT_EQUAL_UINT(REQ_SLOT_TAG_NONE, eraseTag);

	TEST_ASSERT_EQUAL_UINT(lsaBase + USER_PAGES_PER_BLOCK - 1, reqPoolPtr->reqPool[readTag].logicalSliceAddr);
	TEST_ASSERT_EQUAL_UINT(Vorg2VsaTranslation(DIE, block, USER_PAGES_PER_BLOCK - 1), reqPoolPtr->reqPool[readTag].nandInfo.virtualSliceAddr);
	TEST_ASSERT_EQUAL_UINT(REQ_OPT_DATA_BUF_TEMP_ENTRY, reqPoolPtr->reqPool[readTag].reqOpt.dataBufFormat);
	TEST_ASSERT_EQUAL_UINT(REQ_OPT_NAND_ECC_ON, reqPoolPtr->reqPool[readTag].reqOpt.nandEcc);
	TEST_ASSERT_EQUAL_UINT(REQ_OPT_NAND_ECC_WARNING_OFF, reqPoolPtr->reqPool[readTag].reqOpt.nandEccWarning);
	TEST_ASSERT_EQUAL_UINT(REQ_OPT_ROW_ADDR_DEPENDENCY_CHECK, reqPoolPtr->reqPool[readTag].reqOpt.rowAddrDependencyCheck);

	TEST_ASSERT_EQUAL_UINT(lsaBase + USER_PAGES_PER_BLOCK - 1, reqPoolPtr->reqPool[writeTag].logicalSliceAddr);
	TEST_ASSERT_EQUAL_UINT(REQ_OPT_DATA_BUF_TEMP_ENTRY, reqPoolPtr->reqPool[writeTag].reqOpt.dataBufFormat);
	TEST_ASSERT_EQUAL_UINT(reqPoolPtr->reqPool[readTag].dataBufInfo.entry, reqPoolPtr->reqPool[writeTag].dataBufInfo.entry);
	TEST_ASSERT_EQUAL_UINT(reqPoolPtr->reqPool[writeTag].nandInfo.virtualSliceAddr,
	                       logicalSliceMapPtr->logicalSlice[lsaBase + USER_PAGES_PER_BLOCK - 1].virtualSliceAddr);
	TEST_ASSERT_NOT_EQUAL_UINT(block, Vsa2VblockTranslation(reqPoolPtr->reqPool[writeTag].nandInfo.virtualSliceAddr));

	TEST_ASSERT_EQUAL_UINT(Vorg2VsaTranslation(DIE, block, 0), reqPoolPtr->reqPool[eraseTag].nandInfo.virtualSliceAddr);
	TEST_ASSERT_EQUAL_UINT(REQ_OPT_DATA_BUF_NONE, reqPoolPtr->reqPool[eraseTag].reqOpt.dataBufFormat);

	/* Release the NAND and let everything drain. */
	mock_nsc_set_ready_busy(chCtlReg[chNo], 0xffffffffu);
	SyncAllLowLevelReqDone();
	TEST_ASSERT_EQUAL_UINT(0, notCompletedNandReqCnt);
	TEST_ASSERT_EQUAL_UINT(0, blockedByBufDepReqQ.reqCnt);
	TEST_ASSERT_EQUAL_UINT(1, vblock(DIE, block)->free);
}

static void test_gc_on_current_write_block_switches_to_new_block(void)
{
	const unsigned int lsaBase = 500;
	unsigned int currentBlock = virtualDieMapPtr->die[DIE].currentBlock;
	unsigned int lastLsa = lsaBase + USER_PAGES_PER_BLOCK - 1;
	unsigned int vsa;

	fill_block(DIE, currentBlock, lsaBase, USER_PAGES_PER_BLOCK - 1);

	GarbageCollection(DIE);
	SyncAllLowLevelReqDone();

	TEST_ASSERT_NOT_EQUAL_UINT(currentBlock, virtualDieMapPtr->die[DIE].currentBlock);
	vsa = logicalSliceMapPtr->logicalSlice[lastLsa].virtualSliceAddr;
	TEST_ASSERT_EQUAL_UINT(virtualDieMapPtr->die[DIE].currentBlock, Vsa2VblockTranslation(vsa));
	TEST_ASSERT_EQUAL_UINT(0, Vsa2VpageTranslation(vsa));
	TEST_ASSERT_EQUAL_UINT(1, vblock(DIE, currentBlock)->free);
}

static void test_gc_picks_most_invalid_of_several_victims(void)
{
	unsigned int a = take_free_block(DIE), b = take_free_block(DIE);

	fill_block(DIE, a, 600, 2);
	fill_block(DIE, b, 700, USER_PAGES_PER_BLOCK - 1);

	GarbageCollection(DIE);
	SyncAllLowLevelReqDone();

	TEST_ASSERT_EQUAL_UINT(1, vblock(DIE, b)->free);
	TEST_ASSERT_EQUAL_UINT(0, vblock(DIE, a)->free);
	TEST_ASSERT_EQUAL_UINT(a, victim_list(DIE, 2)->headBlock);
}

int main(void)
{
	UNITY_BEGIN();
	RUN_TEST(test_put_first_block_becomes_head_and_tail);
	RUN_TEST(test_put_appends_to_tail_and_links_neighbours);
	RUN_TEST(test_put_keeps_lists_separate_per_die_and_invalid_count);
	RUN_TEST(test_get_returns_most_invalid_block_first);
	RUN_TEST(test_get_pops_head_and_promotes_next_block);
	RUN_TEST(test_get_ignores_zero_invalid_bucket_and_asserts_when_empty);
	RUN_TEST(test_get_asserts_on_empty_victim_list);
	RUN_TEST(test_get_does_not_see_other_dies_victims);
	RUN_TEST(test_selective_get_removes_middle_block);
	RUN_TEST(test_selective_get_removes_tail_block);
	RUN_TEST(test_selective_get_removes_head_block);
	RUN_TEST(test_selective_get_removes_only_block_and_empties_list);
	RUN_TEST(test_invalidating_a_slice_moves_block_to_higher_bucket);
	RUN_TEST(test_gc_on_empty_victim_list_asserts);
	RUN_TEST(test_gc_fully_invalid_block_only_erases);
	RUN_TEST(test_gc_partially_valid_block_copies_valid_slices_then_erases);
	RUN_TEST(test_gc_copy_requests_use_temp_buffers_and_vsa_addressing);
	RUN_TEST(test_gc_on_current_write_block_switches_to_new_block);
	RUN_TEST(test_gc_picks_most_invalid_of_several_victims);
	return UNITY_END();
}
