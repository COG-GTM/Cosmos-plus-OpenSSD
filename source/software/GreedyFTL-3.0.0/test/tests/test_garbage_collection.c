#include "test_support.h"

#define DIE 0

void setUp(void)
{
	test_ftl_init();
}

void tearDown(void)
{
}

/* Take a block off the free list and fill every page with LSAs lsaBase.. through the maps and NAND. */
static unsigned int fillBlock(unsigned int dieNo, unsigned int lsaBase, unsigned char fill)
{
	unsigned int blockNo = GetFromFbList(dieNo, GET_FREE_BLOCK_NORMAL);
	unsigned int pageNo, vsa;

	TEST_ASSERT_NOT_EQUAL_UINT(BLOCK_FAIL, blockNo);
	for (pageNo = 0; pageNo < USER_PAGES_PER_BLOCK; pageNo++)
	{
		vsa = Vorg2VsaTranslation(dieNo, blockNo, pageNo);
		logicalSliceMapPtr->logicalSlice[lsaBase + pageNo].virtualSliceAddr = vsa;
		virtualSliceMapPtr->virtualSlice[vsa].logicalSliceAddr = lsaBase + pageNo;
		test_program_vsa(lsaBase + pageNo, vsa, fill);
	}
	virtualBlockMapPtr->block[dieNo][blockNo].currentPage = USER_PAGES_PER_BLOCK;
	return blockNo;
}

static void invalidateFirst(unsigned int lsaBase, unsigned int count)
{
	unsigned int i;

	for (i = 0; i < count; i++)
		InvalidateOldVsa(lsaBase + i);
}

/* ---- victim list bookkeeping ------------------------------------------------ */

static void test_victim_list_is_empty_after_init(void)
{
	unsigned int dieNo, cnt;

	for (dieNo = 0; dieNo < USER_DIES; dieNo++)
		for (cnt = 0; cnt <= SLICES_PER_BLOCK; cnt++)
		{
			TEST_ASSERT_EQUAL_UINT(BLOCK_NONE, gcVictimMapPtr->gcVictimList[dieNo][cnt].headBlock);
			TEST_ASSERT_EQUAL_UINT(BLOCK_NONE, gcVictimMapPtr->gcVictimList[dieNo][cnt].tailBlock);
		}
}

static void test_put_appends_to_bucket_tail(void)
{
	PutToGcVictimList(DIE, 100, 7);
	PutToGcVictimList(DIE, 200, 7);
	PutToGcVictimList(DIE, 300, 9);

	TEST_ASSERT_EQUAL_UINT(100, gcVictimMapPtr->gcVictimList[DIE][7].headBlock);
	TEST_ASSERT_EQUAL_UINT(200, gcVictimMapPtr->gcVictimList[DIE][7].tailBlock);
	TEST_ASSERT_EQUAL_UINT(200, virtualBlockMapPtr->block[DIE][100].nextBlock);
	TEST_ASSERT_EQUAL_UINT(100, virtualBlockMapPtr->block[DIE][200].prevBlock);
	TEST_ASSERT_EQUAL_UINT(2, test_count_gc_victims(DIE, 7));
	TEST_ASSERT_EQUAL_UINT(1, test_count_gc_victims(DIE, 9));
	TEST_ASSERT_EQUAL_UINT(0, test_count_gc_victims(DIE + 1, 7));
}

static void test_selective_get_unlinks_from_middle_head_and_tail(void)
{
	virtualBlockMapPtr->block[DIE][100].invalidSliceCnt = 3;
	virtualBlockMapPtr->block[DIE][200].invalidSliceCnt = 3;
	virtualBlockMapPtr->block[DIE][300].invalidSliceCnt = 3;
	PutToGcVictimList(DIE, 100, 3);
	PutToGcVictimList(DIE, 200, 3);
	PutToGcVictimList(DIE, 300, 3);

	SelectiveGetFromGcVictimList(DIE, 200);
	TEST_ASSERT_EQUAL_UINT(300, virtualBlockMapPtr->block[DIE][100].nextBlock);
	TEST_ASSERT_EQUAL_UINT(100, virtualBlockMapPtr->block[DIE][300].prevBlock);
	TEST_ASSERT_EQUAL_UINT(2, test_count_gc_victims(DIE, 3));

	SelectiveGetFromGcVictimList(DIE, 100);
	TEST_ASSERT_EQUAL_UINT(300, gcVictimMapPtr->gcVictimList[DIE][3].headBlock);
	TEST_ASSERT_EQUAL_UINT(BLOCK_NONE, virtualBlockMapPtr->block[DIE][300].prevBlock);

	SelectiveGetFromGcVictimList(DIE, 300);
	TEST_ASSERT_EQUAL_UINT(BLOCK_NONE, gcVictimMapPtr->gcVictimList[DIE][3].headBlock);
	TEST_ASSERT_EQUAL_UINT(BLOCK_NONE, gcVictimMapPtr->gcVictimList[DIE][3].tailBlock);
}

/* ---- victim selection ------------------------------------------------------ */

static void test_victim_selection_picks_block_with_fewest_valid_pages(void)
{
	unsigned int mostlyValid = fillBlock(DIE, 0, 0x11);
	unsigned int mostlyInvalid = fillBlock(DIE, 1000, 0x22);
	unsigned int halfInvalid = fillBlock(DIE, 2000, 0x33);

	invalidateFirst(0, 10);
	invalidateFirst(1000, 100);
	invalidateFirst(2000, 64);

	TEST_ASSERT_EQUAL_UINT(USER_PAGES_PER_BLOCK - 10, test_count_valid_slices(DIE, mostlyValid));
	TEST_ASSERT_EQUAL_UINT(USER_PAGES_PER_BLOCK - 100, test_count_valid_slices(DIE, mostlyInvalid));
	TEST_ASSERT_EQUAL_UINT(1, test_count_gc_victims(DIE, 10));
	TEST_ASSERT_EQUAL_UINT(1, test_count_gc_victims(DIE, 100));
	TEST_ASSERT_EQUAL_UINT(1, test_count_gc_victims(DIE, 64));

	TEST_ASSERT_EQUAL_UINT(mostlyInvalid, GetFromGcVictimList(DIE));
	TEST_ASSERT_EQUAL_UINT(halfInvalid, GetFromGcVictimList(DIE));
	TEST_ASSERT_EQUAL_UINT(mostlyValid, GetFromGcVictimList(DIE));
	TEST_ASSERT_EQUAL_UINT(0, test_count_gc_victims(DIE, 100));
}

static void test_victim_selection_is_fifo_within_a_bucket(void)
{
	unsigned int first = fillBlock(DIE, 0, 0x11);
	unsigned int second = fillBlock(DIE, 1000, 0x22);

	invalidateFirst(0, 5);
	invalidateFirst(1000, 5);

	TEST_ASSERT_EQUAL_UINT(first, GetFromGcVictimList(DIE));
	TEST_ASSERT_EQUAL_UINT(second, GetFromGcVictimList(DIE));
}

static void test_victim_selection_ignores_blocks_without_invalid_pages(void)
{
	unsigned int allValid = fillBlock(DIE, 0, 0x11);
	unsigned int oneInvalid = fillBlock(DIE, 1000, 0x22);

	invalidateFirst(1000, 1);

	TEST_ASSERT_EQUAL_UINT(oneInvalid, GetFromGcVictimList(DIE));
	TEST_ASSERT_EQUAL_UINT(0, virtualBlockMapPtr->block[DIE][allValid].invalidSliceCnt);
	TEST_ASSERT_EQUAL_UINT(BLOCK_NONE, gcVictimMapPtr->gcVictimList[DIE][0].headBlock);
}

/* ---- GarbageCollection ---------------------------------------------------- */

static void test_gc_of_fully_invalid_block_erases_without_copying(void)
{
	unsigned int victim = fillBlock(DIE, 0, 0x11);
	unsigned int freeBefore, pagesBefore;
	const FAKE_NAND_STATS *stats;

	invalidateFirst(0, USER_PAGES_PER_BLOCK);
	TEST_ASSERT_EQUAL_UINT(SLICES_PER_BLOCK, virtualBlockMapPtr->block[DIE][victim].invalidSliceCnt);
	freeBefore = virtualDieMapPtr->die[DIE].freeBlockCnt;
	pagesBefore = fake_nand_written_page_count();
	fake_nand_reset_stats();

	GarbageCollection(DIE);
	test_drain_nand();

	stats = fake_nand_stats();
	TEST_ASSERT_EQUAL_UINT(0, stats->readTriggers);
	TEST_ASSERT_EQUAL_UINT(0, stats->readTransfers);
	TEST_ASSERT_EQUAL_UINT(0, stats->programs);
	TEST_ASSERT_EQUAL_UINT(1, stats->erases);
	TEST_ASSERT_EQUAL_UINT(pagesBefore - USER_PAGES_PER_BLOCK, fake_nand_written_page_count());

	TEST_ASSERT_EQUAL_UINT(1, virtualBlockMapPtr->block[DIE][victim].free);
	TEST_ASSERT_EQUAL_UINT(0, virtualBlockMapPtr->block[DIE][victim].invalidSliceCnt);
	TEST_ASSERT_EQUAL_UINT(0, virtualBlockMapPtr->block[DIE][victim].currentPage);
	TEST_ASSERT_EQUAL_UINT(1, virtualBlockMapPtr->block[DIE][victim].eraseCnt);
	TEST_ASSERT_EQUAL_UINT(freeBefore + 1, virtualDieMapPtr->die[DIE].freeBlockCnt);
	TEST_ASSERT_EQUAL_UINT(victim, virtualDieMapPtr->die[DIE].tailFreeBlock);
	TEST_ASSERT_EQUAL_UINT(LSA_NONE, virtualSliceMapPtr->virtualSlice[Vorg2VsaTranslation(DIE, victim, 0)].logicalSliceAddr);
	TEST_ASSERT_EQUAL_UINT(BLOCK_NONE, gcVictimMapPtr->gcVictimList[DIE][SLICES_PER_BLOCK].headBlock);
}

static void test_gc_copies_only_valid_pages_and_remaps_them(void)
{
	const unsigned int invalidCount = USER_PAGES_PER_BLOCK - 8;
	unsigned int victim = fillBlock(DIE, 0, 0x5C);
	unsigned int currentBlock = virtualDieMapPtr->die[DIE].currentBlock;
	unsigned int i, oldVsa[8], newVsa;
	const FAKE_NAND_STATS *stats;
	PSA psa;

	invalidateFirst(0, invalidCount);
	for (i = 0; i < 8; i++)
		oldVsa[i] = logicalSliceMapPtr->logicalSlice[invalidCount + i].virtualSliceAddr;
	TEST_ASSERT_EQUAL_UINT(8, test_count_valid_slices(DIE, victim));
	fake_nand_reset_stats();

	GarbageCollection(DIE);
	test_drain_nand();

	stats = fake_nand_stats();
	TEST_ASSERT_EQUAL_UINT(8, stats->readTriggers);
	TEST_ASSERT_EQUAL_UINT(8, stats->readTransfers);
	TEST_ASSERT_EQUAL_UINT(8, stats->programs);
	TEST_ASSERT_EQUAL_UINT(1, stats->erases);
	TEST_ASSERT_EQUAL_UINT(8, fake_nand_written_page_count());

	for (i = 0; i < 8; i++)
	{
		newVsa = logicalSliceMapPtr->logicalSlice[invalidCount + i].virtualSliceAddr;
		TEST_ASSERT_NOT_EQUAL_UINT(oldVsa[i], newVsa);
		TEST_ASSERT_EQUAL_UINT(DIE, Vsa2VdieTranslation(newVsa));
		TEST_ASSERT_EQUAL_UINT(currentBlock, Vsa2VblockTranslation(newVsa));
		TEST_ASSERT_EQUAL_UINT(i, Vsa2VpageTranslation(newVsa));
		TEST_ASSERT_EQUAL_UINT(invalidCount + i, virtualSliceMapPtr->virtualSlice[newVsa].logicalSliceAddr);

		psa = test_vsa_to_psa(newVsa);
		TEST_ASSERT_NOT_NULL(fake_nand_page_data(psa.chNo, psa.wayNo, psa.rowAddr));
		TEST_ASSERT_EQUAL_HEX8(0x5C, fake_nand_page_data(psa.chNo, psa.wayNo, psa.rowAddr)[0]);
		TEST_ASSERT_EQUAL_HEX8(0x5C, fake_nand_page_data(psa.chNo, psa.wayNo, psa.rowAddr)[BYTES_PER_DATA_REGION_OF_SLICE - 1]);
	}
	for (i = 0; i < invalidCount; i++)
		TEST_ASSERT_EQUAL_UINT(VSA_NONE, logicalSliceMapPtr->logicalSlice[i].virtualSliceAddr);

	TEST_ASSERT_EQUAL_UINT(8, virtualBlockMapPtr->block[DIE][currentBlock].currentPage);
	TEST_ASSERT_EQUAL_UINT(8, test_count_valid_slices(DIE, currentBlock));
	TEST_ASSERT_EQUAL_UINT(1, virtualBlockMapPtr->block[DIE][victim].free);
	TEST_ASSERT_EQUAL_UINT(0, test_count_valid_slices(DIE, victim));
}

static void test_gc_moves_to_new_block_when_victim_is_current_block(void)
{
	unsigned int victim = fillBlock(DIE, 0, 0x77);
	unsigned int nextFree = virtualDieMapPtr->die[DIE].headFreeBlock;
	unsigned int newVsa;

	virtualDieMapPtr->die[DIE].currentBlock = victim;
	invalidateFirst(0, USER_PAGES_PER_BLOCK - 1);

	GarbageCollection(DIE);
	test_drain_nand();

	newVsa = logicalSliceMapPtr->logicalSlice[USER_PAGES_PER_BLOCK - 1].virtualSliceAddr;
	TEST_ASSERT_EQUAL_UINT(nextFree, virtualDieMapPtr->die[DIE].currentBlock);
	TEST_ASSERT_EQUAL_UINT(nextFree, Vsa2VblockTranslation(newVsa));
	TEST_ASSERT_EQUAL_UINT(1, fake_nand_written_page_count());
}

static void test_find_free_slice_triggers_gc_when_free_blocks_are_exhausted(void)
{
	unsigned int victim = fillBlock(DIE, 0, 0x42);
	unsigned int currentBlock = virtualDieMapPtr->die[DIE].currentBlock;
	unsigned int vsa;

	invalidateFirst(0, USER_PAGES_PER_BLOCK);
	virtualBlockMapPtr->block[DIE][currentBlock].currentPage = USER_PAGES_PER_BLOCK;
	virtualDieMapPtr->die[DIE].freeBlockCnt = RESERVED_FREE_BLOCK_COUNT;
	fake_nand_reset_stats();

	sliceAllocationTargetDie = DIE;
	vsa = FindFreeVirtualSlice();
	test_drain_nand();

	TEST_ASSERT_EQUAL_UINT(1, fake_nand_stats()->erases);
	TEST_ASSERT_EQUAL_UINT(1, virtualBlockMapPtr->block[DIE][victim].free);
	TEST_ASSERT_EQUAL_UINT(DIE, Vsa2VdieTranslation(vsa));
	TEST_ASSERT_NOT_EQUAL_UINT(currentBlock, Vsa2VblockTranslation(vsa));
	TEST_ASSERT_EQUAL_UINT(0, Vsa2VpageTranslation(vsa));
	TEST_ASSERT_EQUAL_UINT(1, virtualBlockMapPtr->block[DIE][Vsa2VblockTranslation(vsa)].currentPage);
}

int main(void)
{
	UNITY_BEGIN();
	RUN_TEST(test_victim_list_is_empty_after_init);
	RUN_TEST(test_put_appends_to_bucket_tail);
	RUN_TEST(test_selective_get_unlinks_from_middle_head_and_tail);
	RUN_TEST(test_victim_selection_picks_block_with_fewest_valid_pages);
	RUN_TEST(test_victim_selection_is_fifo_within_a_bucket);
	RUN_TEST(test_victim_selection_ignores_blocks_without_invalid_pages);
	RUN_TEST(test_gc_of_fully_invalid_block_erases_without_copying);
	RUN_TEST(test_gc_copies_only_valid_pages_and_remaps_them);
	RUN_TEST(test_gc_moves_to_new_block_when_victim_is_current_block);
	RUN_TEST(test_find_free_slice_triggers_gc_when_free_blocks_are_exhausted);
	return UNITY_END();
}
