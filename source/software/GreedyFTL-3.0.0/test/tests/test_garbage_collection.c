#include <string.h>

#include "unity.h"

#include "ftl_test_env.h"

#define LSA_BASE 5000u

void setUp(void)
{
	ftl_test_env_init();
}

void tearDown(void)
{
}

static unsigned char PatternFor(unsigned int lsa)
{
	return (unsigned char)(0x40 + (lsa - LSA_BASE) % 64);
}

/*
 * Program one slice per page of the current block on every die (the allocator
 * round-robins dies), remembering which LSA landed on each page of `die`'s
 * current block. Returns that block number.
 */
static unsigned int FillCurrentBlockOfDie(unsigned int die, unsigned int lsaOfPage[USER_PAGES_PER_BLOCK])
{
	unsigned int block = virtualDieMapPtr->die[die].currentBlock;
	unsigned int lsa, i, filled = 0;

	for (lsa = LSA_BASE, i = 0; i < USER_PAGES_PER_BLOCK * USER_DIES; lsa++, i++)
	{
		unsigned int vsa = ftl_test_write_slice(lsa, PatternFor(lsa));
		if (Vsa2VdieTranslation(vsa) == die)
		{
			TEST_ASSERT_EQUAL_UINT32(block, Vsa2VblockTranslation(vsa));
			lsaOfPage[Vsa2VpageTranslation(vsa)] = lsa;
			filled++;
		}
	}
	TEST_ASSERT_EQUAL_UINT32(USER_PAGES_PER_BLOCK, filled);
	TEST_ASSERT_EQUAL_UINT32(USER_PAGES_PER_BLOCK, virtualBlockMapPtr->block[die][block].currentPage);
	return block;
}

static FAKE_NAND_DIE_STATS StatsOfDie(unsigned int die)
{
	return fake_nand_stats(Vdie2PchTranslation(die), Vdie2PwayTranslation(die));
}

/* ---- Victim selection --------------------------------------------------- */

static void test_victim_selection_prefers_block_with_fewest_valid_slices(void)
{
	unsigned int die = 1;

	virtualBlockMapPtr->block[die][10].invalidSliceCnt = 3;
	virtualBlockMapPtr->block[die][11].invalidSliceCnt = SLICES_PER_BLOCK - 1;
	virtualBlockMapPtr->block[die][12].invalidSliceCnt = 7;
	PutToGcVictimList(die, 10, 3);
	PutToGcVictimList(die, 11, SLICES_PER_BLOCK - 1);
	PutToGcVictimList(die, 12, 7);

	TEST_ASSERT_EQUAL_UINT32(11, GetFromGcVictimList(die));
	TEST_ASSERT_EQUAL_UINT32(12, GetFromGcVictimList(die));
	TEST_ASSERT_EQUAL_UINT32(10, GetFromGcVictimList(die));
	TEST_ASSERT_EQUAL_UINT32(BLOCK_NONE, gcVictimMapPtr->gcVictimList[die][3].headBlock);
	TEST_ASSERT_EQUAL_UINT32(BLOCK_NONE, gcVictimMapPtr->gcVictimList[die][7].headBlock);
	TEST_ASSERT_EQUAL_UINT32(BLOCK_NONE, gcVictimMapPtr->gcVictimList[die][SLICES_PER_BLOCK - 1].headBlock);
}

static void test_victim_list_is_fifo_within_same_invalid_count(void)
{
	unsigned int die = 0;

	PutToGcVictimList(die, 20, 5);
	PutToGcVictimList(die, 21, 5);
	PutToGcVictimList(die, 22, 5);

	TEST_ASSERT_EQUAL_UINT32(20, gcVictimMapPtr->gcVictimList[die][5].headBlock);
	TEST_ASSERT_EQUAL_UINT32(22, gcVictimMapPtr->gcVictimList[die][5].tailBlock);
	TEST_ASSERT_EQUAL_UINT32(21, virtualBlockMapPtr->block[die][20].nextBlock);
	TEST_ASSERT_EQUAL_UINT32(20, virtualBlockMapPtr->block[die][21].prevBlock);

	TEST_ASSERT_EQUAL_UINT32(20, GetFromGcVictimList(die));
	TEST_ASSERT_EQUAL_UINT32(21, gcVictimMapPtr->gcVictimList[die][5].headBlock);
	TEST_ASSERT_EQUAL_UINT32(BLOCK_NONE, virtualBlockMapPtr->block[die][21].prevBlock);
}

static void test_selective_removal_unlinks_middle_head_and_tail(void)
{
	unsigned int die = 0, b;

	for (b = 30; b < 33; b++)
	{
		virtualBlockMapPtr->block[die][b].invalidSliceCnt = 9;
		PutToGcVictimList(die, b, 9);
	}

	SelectiveGetFromGcVictimList(die, 31); /* middle */
	TEST_ASSERT_EQUAL_UINT32(32, virtualBlockMapPtr->block[die][30].nextBlock);
	TEST_ASSERT_EQUAL_UINT32(30, virtualBlockMapPtr->block[die][32].prevBlock);

	SelectiveGetFromGcVictimList(die, 32); /* tail */
	TEST_ASSERT_EQUAL_UINT32(30, gcVictimMapPtr->gcVictimList[die][9].tailBlock);
	TEST_ASSERT_EQUAL_UINT32(BLOCK_NONE, virtualBlockMapPtr->block[die][30].nextBlock);

	SelectiveGetFromGcVictimList(die, 30); /* last one */
	TEST_ASSERT_EQUAL_UINT32(BLOCK_NONE, gcVictimMapPtr->gcVictimList[die][9].headBlock);
	TEST_ASSERT_EQUAL_UINT32(BLOCK_NONE, gcVictimMapPtr->gcVictimList[die][9].tailBlock);
}

static void test_victim_list_ignores_blocks_with_no_invalid_slices(void)
{
	unsigned int die = 2;
	unsigned int lsaOfPage[USER_PAGES_PER_BLOCK];

	/* A fully written but fully valid block is never a GC candidate. */
	FillCurrentBlockOfDie(die, lsaOfPage);
	TEST_ASSERT_EQUAL_UINT32(BLOCK_NONE, gcVictimMapPtr->gcVictimList[die][0].headBlock);
	InvalidateOldVsa(lsaOfPage[0]);
	TEST_ASSERT_NOT_EQUAL(BLOCK_NONE, gcVictimMapPtr->gcVictimList[die][1].headBlock);
}

/* ---- GarbageCollection() ------------------------------------------------ */

static void test_gc_of_fully_invalid_block_erases_without_copies(void)
{
	unsigned int die = 0, block, page;
	unsigned int lsaOfPage[USER_PAGES_PER_BLOCK];
	unsigned int freeBefore, eraseBefore;
	FAKE_NAND_DIE_STATS before, after;

	block = FillCurrentBlockOfDie(die, lsaOfPage);
	for (page = 0; page < USER_PAGES_PER_BLOCK; page++)
		InvalidateOldVsa(lsaOfPage[page]);
	TEST_ASSERT_EQUAL_UINT32(SLICES_PER_BLOCK, virtualBlockMapPtr->block[die][block].invalidSliceCnt);
	TEST_ASSERT_EQUAL_UINT32(block, gcVictimMapPtr->gcVictimList[die][SLICES_PER_BLOCK].headBlock);

	freeBefore = virtualDieMapPtr->die[die].freeBlockCnt;
	eraseBefore = virtualBlockMapPtr->block[die][block].eraseCnt;
	before = StatsOfDie(die);

	GarbageCollection(die);
	ftl_test_drain();
	after = StatsOfDie(die);

	TEST_ASSERT_EQUAL_UINT32(before.programs, after.programs);
	TEST_ASSERT_EQUAL_UINT32(before.readTransfers, after.readTransfers);
	TEST_ASSERT_EQUAL_UINT32(before.erases + 1, after.erases);

	TEST_ASSERT_EQUAL_UINT32(1, virtualBlockMapPtr->block[die][block].free);
	TEST_ASSERT_EQUAL_UINT32(0, virtualBlockMapPtr->block[die][block].currentPage);
	TEST_ASSERT_EQUAL_UINT32(0, virtualBlockMapPtr->block[die][block].invalidSliceCnt);
	TEST_ASSERT_EQUAL_UINT32(eraseBefore + 1, virtualBlockMapPtr->block[die][block].eraseCnt);
	TEST_ASSERT_EQUAL_UINT32(freeBefore + 1, virtualDieMapPtr->die[die].freeBlockCnt);
	TEST_ASSERT_EQUAL_UINT32(block, virtualDieMapPtr->die[die].tailFreeBlock);
	TEST_ASSERT_EQUAL_UINT32(BLOCK_NONE, gcVictimMapPtr->gcVictimList[die][SLICES_PER_BLOCK].headBlock);

	for (page = 0; page < USER_PAGES_PER_BLOCK; page++)
		TEST_ASSERT_EQUAL_HEX32(LSA_NONE, virtualSliceMapPtr->virtualSlice[ftl_test_vsa(die, block, page)].logicalSliceAddr);
}

static void test_gc_copies_only_valid_slices_and_rehomes_them(void)
{
	unsigned int die = 0, block, page;
	unsigned int lsaOfPage[USER_PAGES_PER_BLOCK];
	unsigned int validCount = 0;
	FAKE_NAND_DIE_STATS before, after;
	unsigned char pattern[BYTES_PER_DATA_REGION_OF_PAGE];

	block = FillCurrentBlockOfDie(die, lsaOfPage);

	/* Every odd page stays valid. */
	for (page = 0; page < USER_PAGES_PER_BLOCK; page++)
	{
		if (page % 2 == 0)
			InvalidateOldVsa(lsaOfPage[page]);
		else
			validCount++;
	}
	TEST_ASSERT_EQUAL_UINT32(SLICES_PER_BLOCK - validCount, virtualBlockMapPtr->block[die][block].invalidSliceCnt);

	before = StatsOfDie(die);
	GarbageCollection(die);
	ftl_test_drain();
	after = StatsOfDie(die);

	TEST_ASSERT_EQUAL_UINT32(validCount, after.readTransfers - before.readTransfers);
	TEST_ASSERT_EQUAL_UINT32(validCount, after.programs - before.programs);
	TEST_ASSERT_EQUAL_UINT32(1, after.erases - before.erases);

	/* Victim block is back in the free pool, and valid data lives elsewhere on the same die. */
	TEST_ASSERT_EQUAL_UINT32(1, virtualBlockMapPtr->block[die][block].free);
	for (page = 0; page < USER_PAGES_PER_BLOCK; page++)
	{
		unsigned int vsa = AddrTransRead(lsaOfPage[page]);
		if (page % 2 == 0)
		{
			TEST_ASSERT_EQUAL_HEX32(VSA_FAIL, vsa);
		}
		else
		{
			unsigned int newBlock = Vsa2VblockTranslation(vsa);

			TEST_ASSERT_EQUAL_UINT32(die, Vsa2VdieTranslation(vsa));
			TEST_ASSERT_NOT_EQUAL(block, newBlock);
			TEST_ASSERT_EQUAL_UINT32(lsaOfPage[page], virtualSliceMapPtr->virtualSlice[vsa].logicalSliceAddr);

			memset(pattern, PatternFor(lsaOfPage[page]), sizeof(pattern));
			TEST_ASSERT_EQUAL_MEMORY(pattern, ftl_test_nand_page(vsa), BYTES_PER_DATA_REGION_OF_PAGE);
		}
	}
}

static void test_gc_moves_current_block_pointer_when_victim_is_current(void)
{
	unsigned int die = 3, block, page, newCurrent;
	unsigned int lsaOfPage[USER_PAGES_PER_BLOCK];

	block = FillCurrentBlockOfDie(die, lsaOfPage);
	TEST_ASSERT_EQUAL_UINT32(block, virtualDieMapPtr->die[die].currentBlock);
	for (page = 1; page < USER_PAGES_PER_BLOCK; page++)
		InvalidateOldVsa(lsaOfPage[page]);

	GarbageCollection(die);
	ftl_test_drain();

	newCurrent = virtualDieMapPtr->die[die].currentBlock;
	TEST_ASSERT_NOT_EQUAL(block, newCurrent);
	TEST_ASSERT_EQUAL_UINT32(0, virtualBlockMapPtr->block[die][newCurrent].free);
	TEST_ASSERT_EQUAL_UINT32(1, virtualBlockMapPtr->block[die][newCurrent].currentPage);
	TEST_ASSERT_EQUAL_UINT32(newCurrent, Vsa2VblockTranslation(AddrTransRead(lsaOfPage[0])));
}

static void test_allocation_triggers_gc_when_free_blocks_are_exhausted(void)
{
	unsigned int die = 0, block, page;
	unsigned int lsaOfPage[USER_PAGES_PER_BLOCK];
	unsigned int vsa;

	block = FillCurrentBlockOfDie(die, lsaOfPage);
	for (page = 0; page < USER_PAGES_PER_BLOCK; page++)
		InvalidateOldVsa(lsaOfPage[page]);

	/* Drain the free list down to the GC reserve so the next allocation must reclaim. */
	while (GetFromFbList(die, GET_FREE_BLOCK_NORMAL) != BLOCK_FAIL)
		;
	TEST_ASSERT_EQUAL_UINT32(RESERVED_FREE_BLOCK_COUNT, virtualDieMapPtr->die[die].freeBlockCnt);

	/* Advance the allocator cursor to `die` and ask for one more slice. */
	while (sliceAllocationTargetDie != die)
		FindFreeVirtualSlice();
	vsa = FindFreeVirtualSlice();
	ftl_test_drain();

	TEST_ASSERT_EQUAL_UINT32(die, Vsa2VdieTranslation(vsa));
	TEST_ASSERT_EQUAL_UINT32(block, Vsa2VblockTranslation(vsa));
	TEST_ASSERT_EQUAL_UINT32(0, Vsa2VpageTranslation(vsa));
	TEST_ASSERT_EQUAL_UINT32(1, virtualBlockMapPtr->block[die][block].eraseCnt);
}

int main(void)
{
	UNITY_BEGIN();
	RUN_TEST(test_victim_selection_prefers_block_with_fewest_valid_slices);
	RUN_TEST(test_victim_list_is_fifo_within_same_invalid_count);
	RUN_TEST(test_selective_removal_unlinks_middle_head_and_tail);
	RUN_TEST(test_victim_list_ignores_blocks_with_no_invalid_slices);
	RUN_TEST(test_gc_of_fully_invalid_block_erases_without_copies);
	RUN_TEST(test_gc_copies_only_valid_slices_and_rehomes_them);
	RUN_TEST(test_gc_moves_current_block_pointer_when_victim_is_current);
	RUN_TEST(test_allocation_triggers_gc_when_free_blocks_are_exhausted);
	return UNITY_END();
}
