#include "test_support.h"

void setUp(void) { TestFtlReset(); }
void tearDown(void) {}

/* With a fresh FTL the allocator round-robins dies, so LSA i lands on die i % USER_DIES
 * and page i / USER_DIES of that die's current block. Writing one full "stripe" of
 * USER_DIES * USER_PAGES_PER_BLOCK slices therefore fills exactly one block per die. */
#define STRIPE (USER_DIES * USER_PAGES_PER_BLOCK)

static unsigned int FreeReqsInUse(void)
{
	return AVAILABLE_OUNTSTANDING_REQ_COUNT - freeReqQ.reqCnt;
}

static unsigned int CountValidSlicesInBlock(unsigned int die, unsigned int block)
{
	unsigned int page, n = 0;
	for (page = 0; page < USER_PAGES_PER_BLOCK; page++)
	{
		unsigned int vsa = Vorg2VsaTranslation(die, block, page);
		unsigned int lsa = virtualSliceMapPtr->virtualSlice[vsa].logicalSliceAddr;
		if (lsa != LSA_NONE && logicalSliceMapPtr->logicalSlice[lsa].virtualSliceAddr == vsa)
			n++;
	}
	return n;
}

/* ---------------------------------------------------------------- victim list */

void test_victim_map_starts_empty(void)
{
	unsigned int die, cnt;
	for (die = 0; die < USER_DIES; die++)
		for (cnt = 0; cnt <= SLICES_PER_BLOCK; cnt++)
		{
			TEST_ASSERT_EQUAL_UINT(BLOCK_NONE, gcVictimMapPtr->gcVictimList[die][cnt].headBlock);
			TEST_ASSERT_EQUAL_UINT(BLOCK_NONE, gcVictimMapPtr->gcVictimList[die][cnt].tailBlock);
		}
}

void test_put_to_victim_list_links_blocks_in_fifo_order(void)
{
	PutToGcVictimList(0, 3, 7);
	PutToGcVictimList(0, 8, 7);
	PutToGcVictimList(0, 5, 7);

	TEST_ASSERT_EQUAL_UINT(3, gcVictimMapPtr->gcVictimList[0][7].headBlock);
	TEST_ASSERT_EQUAL_UINT(5, gcVictimMapPtr->gcVictimList[0][7].tailBlock);
	TEST_ASSERT_EQUAL_UINT(8, virtualBlockMapPtr->block[0][3].nextBlock);
	TEST_ASSERT_EQUAL_UINT(3, virtualBlockMapPtr->block[0][8].prevBlock);
	TEST_ASSERT_EQUAL_UINT(5, virtualBlockMapPtr->block[0][8].nextBlock);
	TEST_ASSERT_EQUAL_UINT(BLOCK_NONE, virtualBlockMapPtr->block[0][5].nextBlock);
}

void test_selective_get_unlinks_middle_head_and_tail(void)
{
	PutToGcVictimList(1, 3, 4);
	PutToGcVictimList(1, 8, 4);
	PutToGcVictimList(1, 5, 4);
	virtualBlockMapPtr->block[1][3].invalidSliceCnt = 4;
	virtualBlockMapPtr->block[1][8].invalidSliceCnt = 4;
	virtualBlockMapPtr->block[1][5].invalidSliceCnt = 4;

	SelectiveGetFromGcVictimList(1, 8);
	TEST_ASSERT_EQUAL_UINT(5, virtualBlockMapPtr->block[1][3].nextBlock);
	TEST_ASSERT_EQUAL_UINT(3, virtualBlockMapPtr->block[1][5].prevBlock);

	SelectiveGetFromGcVictimList(1, 3);
	TEST_ASSERT_EQUAL_UINT(5, gcVictimMapPtr->gcVictimList[1][4].headBlock);
	TEST_ASSERT_EQUAL_UINT(BLOCK_NONE, virtualBlockMapPtr->block[1][5].prevBlock);

	SelectiveGetFromGcVictimList(1, 5);
	TEST_ASSERT_EQUAL_UINT(BLOCK_NONE, gcVictimMapPtr->gcVictimList[1][4].headBlock);
	TEST_ASSERT_EQUAL_UINT(BLOCK_NONE, gcVictimMapPtr->gcVictimList[1][4].tailBlock);
}

void test_victim_selection_prefers_block_with_most_invalid_slices(void)
{
	PutToGcVictimList(0, 10, 3);
	PutToGcVictimList(0, 11, SLICES_PER_BLOCK);
	PutToGcVictimList(0, 12, 50);

	TEST_ASSERT_EQUAL_UINT(11, GetFromGcVictimList(0));
	TEST_ASSERT_EQUAL_UINT(12, GetFromGcVictimList(0));
	TEST_ASSERT_EQUAL_UINT(10, GetFromGcVictimList(0));
	TEST_ASSERT_EQUAL_UINT(BLOCK_NONE, gcVictimMapPtr->gcVictimList[0][3].headBlock);
}

void test_victim_selection_is_fifo_within_same_invalid_count(void)
{
	PutToGcVictimList(2, 20, 9);
	PutToGcVictimList(2, 21, 9);

	TEST_ASSERT_EQUAL_UINT(20, GetFromGcVictimList(2));
	TEST_ASSERT_EQUAL_UINT(21, gcVictimMapPtr->gcVictimList[2][9].headBlock);
	TEST_ASSERT_EQUAL_UINT(BLOCK_NONE, virtualBlockMapPtr->block[2][21].prevBlock);
	TEST_ASSERT_EQUAL_UINT(21, GetFromGcVictimList(2));
}

void test_victim_selection_uses_live_invalid_counts_from_writes(void)
{
	unsigned int die = 0, lsa;
	unsigned int blockA = virtualDieMapPtr->die[die].currentBlock;
	unsigned int blockB;

	TestWriteLogicalSlices(0, 2 * STRIPE);            /* blocks A and B full on every die */
	blockB = virtualDieMapPtr->die[die].currentBlock;
	TEST_ASSERT_NOT_EQUAL(blockA, blockB);

	for (lsa = 0; lsa < STRIPE; lsa += USER_DIES * 4)  /* 32 slices of A invalidated */
		AddrTransWrite(lsa);
	for (lsa = STRIPE; lsa < 2 * STRIPE; lsa += USER_DIES * 2) /* 64 slices of B invalidated */
		AddrTransWrite(lsa);

	TEST_ASSERT_EQUAL_UINT(USER_PAGES_PER_BLOCK / 4, virtualBlockMapPtr->block[die][blockA].invalidSliceCnt);
	TEST_ASSERT_EQUAL_UINT(USER_PAGES_PER_BLOCK / 2, virtualBlockMapPtr->block[die][blockB].invalidSliceCnt);
	TEST_ASSERT_EQUAL_UINT(blockB, GetFromGcVictimList(die));
	TEST_ASSERT_EQUAL_UINT(blockA, GetFromGcVictimList(die));
}

/* ---------------------------------------------------------------- GarbageCollection() */

void test_gc_of_fully_invalid_block_erases_without_copying(void)
{
	unsigned int die = 0;
	unsigned int victim = virtualDieMapPtr->die[die].currentBlock;
	unsigned int freeBefore, reqsBefore;

	TestWriteLogicalSlices(0, STRIPE);
	TestWriteLogicalSlices(0, STRIPE); /* overwrite everything: first block per die fully invalid */
	TEST_ASSERT_EQUAL_UINT(SLICES_PER_BLOCK, virtualBlockMapPtr->block[die][victim].invalidSliceCnt);
	TEST_ASSERT_EQUAL_UINT(0, CountValidSlicesInBlock(die, victim));

	freeBefore = virtualDieMapPtr->die[die].freeBlockCnt;
	reqsBefore = FreeReqsInUse();
	GarbageCollection(die);

	TEST_ASSERT_EQUAL_UINT(reqsBefore + 1, FreeReqsInUse()); /* exactly one request: the erase */
	TEST_ASSERT_EQUAL_UINT(0, TestCountNandReqs(TestChannelOfDie(die), TestWayOfDie(die), REQ_CODE_READ));
	TEST_ASSERT_EQUAL_UINT(0, TestCountNandReqs(TestChannelOfDie(die), TestWayOfDie(die), REQ_CODE_WRITE));
	TEST_ASSERT_EQUAL_UINT(freeBefore + 1, virtualDieMapPtr->die[die].freeBlockCnt);
	TEST_ASSERT_EQUAL_UINT(1, virtualBlockMapPtr->block[die][victim].free);
	TEST_ASSERT_EQUAL_UINT(0, virtualBlockMapPtr->block[die][victim].invalidSliceCnt);
	TEST_ASSERT_EQUAL_UINT(1, virtualBlockMapPtr->block[die][victim].eraseCnt);
	TEST_ASSERT_EQUAL_UINT(BLOCK_NONE, gcVictimMapPtr->gcVictimList[die][SLICES_PER_BLOCK].headBlock);
}

void test_gc_copies_only_valid_slices_and_remaps_them(void)
{
	unsigned int die = 0, lsa, page;
	unsigned int victim = virtualDieMapPtr->die[die].currentBlock;
	unsigned int validCount = 0, reqsBefore;

	TestWriteLogicalSlices(0, STRIPE);
	/* Invalidate the odd pages of the victim by overwriting those LSAs. */
	for (lsa = USER_DIES; lsa < STRIPE; lsa += 2 * USER_DIES)
		AddrTransWrite(lsa);

	validCount = CountValidSlicesInBlock(die, victim);
	TEST_ASSERT_EQUAL_UINT(USER_PAGES_PER_BLOCK / 2, validCount);
	TEST_ASSERT_EQUAL_UINT(USER_PAGES_PER_BLOCK / 2, virtualBlockMapPtr->block[die][victim].invalidSliceCnt);

	reqsBefore = FreeReqsInUse();
	GarbageCollection(die);

	/* One read + one write per valid slice, plus the erase. */
	TEST_ASSERT_EQUAL_UINT(reqsBefore + 2 * validCount + 1, FreeReqsInUse());
	TEST_ASSERT_EQUAL_UINT(1, virtualBlockMapPtr->block[die][victim].free);

	for (page = 0; page < USER_PAGES_PER_BLOCK; page++)
	{
		lsa = page * USER_DIES;
		if (page % 2 == 0)
		{
			unsigned int newVsa = logicalSliceMapPtr->logicalSlice[lsa].virtualSliceAddr;
			TEST_ASSERT_NOT_EQUAL(victim, TestBlockOf(newVsa));
			TEST_ASSERT_EQUAL_UINT(die, TestDieOf(newVsa));
			TEST_ASSERT_EQUAL_UINT(lsa, virtualSliceMapPtr->virtualSlice[newVsa].logicalSliceAddr);
		}
		TEST_ASSERT_EQUAL_HEX32(LSA_NONE, virtualSliceMapPtr->virtualSlice[Vorg2VsaTranslation(die, victim, page)].logicalSliceAddr);
	}
}

void test_gc_copy_targets_stay_on_same_die(void)
{
	unsigned int die = 3, lsa, page;
	unsigned int victim = virtualDieMapPtr->die[die].currentBlock;

	TestWriteLogicalSlices(0, STRIPE);
	for (lsa = die; lsa < STRIPE; lsa += 4 * USER_DIES)
		InvalidateOldVsa(lsa);                           /* every 4th page of the victim dies */

	GarbageCollection(die);

	TEST_ASSERT_EQUAL_UINT(1, virtualBlockMapPtr->block[die][victim].free);
	for (page = 0; page < USER_PAGES_PER_BLOCK; page++)
	{
		unsigned int vsa = logicalSliceMapPtr->logicalSlice[die + page * USER_DIES].virtualSliceAddr;
		if (page % 4 == 0)
			TEST_ASSERT_EQUAL_HEX32(VSA_NONE, vsa);
		else
		{
			TEST_ASSERT_EQUAL_UINT(die, TestDieOf(vsa));
			TEST_ASSERT_NOT_EQUAL(victim, TestBlockOf(vsa));
		}
	}
}

void test_gc_uses_reserved_block_when_victim_is_current_block(void)
{
	unsigned int die = 0, lsa;
	unsigned int block;

	/* Exhaust the normal free blocks so the die is down to its GC reserve. */
	while (GetFromFbList(die, GET_FREE_BLOCK_NORMAL) != BLOCK_FAIL)
		;
	TEST_ASSERT_EQUAL_UINT(RESERVED_FREE_BLOCK_COUNT, virtualDieMapPtr->die[die].freeBlockCnt);

	block = virtualDieMapPtr->die[die].currentBlock;
	TestWriteLogicalSlices(0, USER_DIES * 4); /* four pages into the current block of each die */
	for (lsa = 0; lsa < USER_DIES * 4; lsa += 2 * USER_DIES)
		InvalidateOldVsa(lsa);              /* pages 0 and 2 dead, 1 and 3 live */

	GarbageCollection(die);

	TEST_ASSERT_NOT_EQUAL(block, virtualDieMapPtr->die[die].currentBlock);
	TEST_ASSERT_EQUAL_UINT(1, virtualBlockMapPtr->block[die][block].free);
	TEST_ASSERT_EQUAL_UINT(2, virtualBlockMapPtr->block[die][virtualDieMapPtr->die[die].currentBlock].currentPage);
	TEST_ASSERT_EQUAL_UINT(die, TestDieOf(logicalSliceMapPtr->logicalSlice[USER_DIES].virtualSliceAddr));
	TEST_ASSERT_EQUAL_HEX32(VSA_NONE, logicalSliceMapPtr->logicalSlice[0].virtualSliceAddr);
}

void test_find_free_virtual_slice_triggers_gc_when_out_of_blocks(void)
{
	unsigned int die = 0, lsa, vsa;
	unsigned int reqsBefore;

	while (GetFromFbList(die, GET_FREE_BLOCK_NORMAL) != BLOCK_FAIL)
		;
	TestWriteLogicalSlices(0, STRIPE);                 /* current block of die 0 is now full */
	for (lsa = 0; lsa < STRIPE; lsa += USER_DIES)
		InvalidateOldVsa(lsa);                          /* ...and completely dead */

	reqsBefore = FreeReqsInUse();
	TestRewindSliceAllocationDie();
	vsa = FindFreeVirtualSlice();

	TEST_ASSERT_EQUAL_UINT(die, TestDieOf(vsa));
	TEST_ASSERT_EQUAL_UINT(0, TestPageOf(vsa));
	TEST_ASSERT_EQUAL_UINT(reqsBefore + 1, FreeReqsInUse()); /* GC ran and only had to erase */
}

int main(void)
{
	UNITY_BEGIN();
	RUN_TEST(test_victim_map_starts_empty);
	RUN_TEST(test_put_to_victim_list_links_blocks_in_fifo_order);
	RUN_TEST(test_selective_get_unlinks_middle_head_and_tail);
	RUN_TEST(test_victim_selection_prefers_block_with_most_invalid_slices);
	RUN_TEST(test_victim_selection_is_fifo_within_same_invalid_count);
	RUN_TEST(test_victim_selection_uses_live_invalid_counts_from_writes);
	RUN_TEST(test_gc_of_fully_invalid_block_erases_without_copying);
	RUN_TEST(test_gc_copies_only_valid_slices_and_remaps_them);
	RUN_TEST(test_gc_copy_targets_stay_on_same_die);
	RUN_TEST(test_gc_uses_reserved_block_when_victim_is_current_block);
	RUN_TEST(test_find_free_virtual_slice_triggers_gc_when_out_of_blocks);
	return UNITY_END();
}
