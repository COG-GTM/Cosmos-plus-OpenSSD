#include <string.h>

#include "unity.h"

#include "ftl_test_env.h"

/* Defined in address_translation.c without a header declaration. */
void EraseTotalBlockSpace(void);

/*
 * Bad-block table (BBT) handling in address_translation.c: remapping of
 * factory bad blocks onto the reserved (extended) block area, rebuilding a
 * missing table by scanning the bad-block marks, and flushing grown bad
 * blocks back to NAND.
 */

#define BBT_BLOCK 0u
#define DIE0_CH 0u
#define DIE0_WAY 0u

/*
 * InitBlockDieMap() marks the BBT block (physical block 0) bad so the host can
 * never reach it, which always consumes the first reserved block of LUN0.
 */
#define LUN0_FIRST_RESERVED (USER_BLOCKS_PER_LUN)
#define LUN0_FIRST_FREE_RESERVED (LUN0_FIRST_RESERVED + 1)
#define LUN1_FIRST_RESERVED (TOTAL_BLOCKS_PER_LUN + USER_BLOCKS_PER_LUN)

void setUp(void)
{
}

void tearDown(void)
{
}

/* Flip a table entry of the preloaded on-NAND BBT without touching the block itself. */
static void MarkBadInTable(unsigned int ch, unsigned int way, unsigned int phyBlock)
{
	*ftl_test_bbt_entry(ch, way, BBT_BLOCK, phyBlock) = BLOCK_STATE_BAD;
}

static void BringUpWithTableEdits(void (*edit)(void))
{
	ftl_test_env_reset_fakes();
	ftl_test_env_preload_bbt_except(-1, -1);
	edit();
	ftl_test_env_bring_up();
}

static unsigned int CountFreeBlocks(unsigned int die)
{
	unsigned int block = virtualDieMapPtr->die[die].headFreeBlock, n = 0;
	while (block != BLOCK_NONE)
	{
		n++;
		block = virtualBlockMapPtr->block[die][block].nextBlock;
	}
	return n;
}

/* ---- RemapBadBlock ------------------------------------------------------- */

static void EditLun1Bad(void)
{
	MarkBadInTable(DIE0_CH, DIE0_WAY, TOTAL_BLOCKS_PER_LUN + 7);
}

static void test_lun1_bad_block_is_remapped_onto_lun1_reserved_area(void)
{
	unsigned int die;

	BringUpWithTableEdits(EditLun1Bad);

	TEST_ASSERT_EQUAL_UINT32(BLOCK_STATE_BAD, phyBlockMapPtr->phyBlock[0][TOTAL_BLOCKS_PER_LUN + 7].bad);
	TEST_ASSERT_EQUAL_UINT32(LUN1_FIRST_RESERVED, phyBlockMapPtr->phyBlock[0][TOTAL_BLOCKS_PER_LUN + 7].remappedPhyBlock);
	TEST_ASSERT_EQUAL_UINT32(BLOCK_STATE_NORMAL, virtualBlockMapPtr->block[0][USER_BLOCKS_PER_LUN + 7].bad);
	TEST_ASSERT_EQUAL_UINT32(0, mbPerbadBlockSpace);

	for (die = 1; die < USER_DIES; die++)
		TEST_ASSERT_EQUAL_UINT32(TOTAL_BLOCKS_PER_LUN + 7, phyBlockMapPtr->phyBlock[die][TOTAL_BLOCKS_PER_LUN + 7].remappedPhyBlock);
}

static void EditBadReservedBlocks(void)
{
	MarkBadInTable(DIE0_CH, DIE0_WAY, 3);
	MarkBadInTable(DIE0_CH, DIE0_WAY, LUN0_FIRST_RESERVED);
	MarkBadInTable(DIE0_CH, DIE0_WAY, TOTAL_BLOCKS_PER_LUN + 3);
	MarkBadInTable(DIE0_CH, DIE0_WAY, LUN1_FIRST_RESERVED);
	MarkBadInTable(DIE0_CH, DIE0_WAY, LUN1_FIRST_RESERVED + 1);
}

static void test_remap_skips_reserved_blocks_that_are_themselves_bad(void)
{
	BringUpWithTableEdits(EditBadReservedBlocks);

	/* Block 0 (BBT) skips the bad 4096 and takes 4097; block 3 then takes 4098. */
	TEST_ASSERT_EQUAL_UINT32(LUN0_FIRST_RESERVED + 1, phyBlockMapPtr->phyBlock[0][BBT_BLOCK].remappedPhyBlock);
	TEST_ASSERT_EQUAL_UINT32(LUN0_FIRST_RESERVED + 2, phyBlockMapPtr->phyBlock[0][3].remappedPhyBlock);
	TEST_ASSERT_EQUAL_UINT32(LUN1_FIRST_RESERVED + 2, phyBlockMapPtr->phyBlock[0][TOTAL_BLOCKS_PER_LUN + 3].remappedPhyBlock);
	TEST_ASSERT_EQUAL_UINT32(0, mbPerbadBlockSpace);
	TEST_ASSERT_EQUAL_UINT32(LUN0_FIRST_RESERVED, phyBlockMapPtr->phyBlock[1][BBT_BLOCK].remappedPhyBlock);
}

static void EditExhaustReserved(void)
{
	unsigned int i;
	for (i = 0; i < EXTENDED_BLOCKS_PER_LUN; i++)
	{
		MarkBadInTable(DIE0_CH, DIE0_WAY, LUN0_FIRST_RESERVED + i);
		MarkBadInTable(DIE0_CH, DIE0_WAY, LUN1_FIRST_RESERVED + i);
	}
	MarkBadInTable(DIE0_CH, DIE0_WAY, 10);
	MarkBadInTable(DIE0_CH, DIE0_WAY, 11);
	MarkBadInTable(DIE0_CH, DIE0_WAY, TOTAL_BLOCKS_PER_LUN + 10);
	MarkBadInTable(DIE0_CH, DIE0_WAY, TOTAL_BLOCKS_PER_LUN + 11);
}

static void test_bad_blocks_without_reserved_spares_stay_bad_and_shrink_capacity(void)
{
	BringUpWithTableEdits(EditExhaustReserved);

	TEST_ASSERT_EQUAL_UINT32(10, phyBlockMapPtr->phyBlock[0][10].remappedPhyBlock);
	TEST_ASSERT_EQUAL_UINT32(11, phyBlockMapPtr->phyBlock[0][11].remappedPhyBlock);
	TEST_ASSERT_EQUAL_UINT32(TOTAL_BLOCKS_PER_LUN + 10, phyBlockMapPtr->phyBlock[0][TOTAL_BLOCKS_PER_LUN + 10].remappedPhyBlock);

	TEST_ASSERT_EQUAL_UINT32(BLOCK_STATE_BAD, virtualBlockMapPtr->block[0][10].bad);
	TEST_ASSERT_EQUAL_UINT32(BLOCK_STATE_BAD, virtualBlockMapPtr->block[0][11].bad);
	TEST_ASSERT_EQUAL_UINT32(BLOCK_STATE_BAD, virtualBlockMapPtr->block[0][USER_BLOCKS_PER_LUN + 10].bad);
	TEST_ASSERT_EQUAL_UINT32(BLOCK_STATE_BAD, virtualBlockMapPtr->block[0][USER_BLOCKS_PER_LUN + 11].bad);
	TEST_ASSERT_EQUAL_UINT32(BLOCK_NONE, virtualBlockMapPtr->block[0][10].nextBlock);
	TEST_ASSERT_EQUAL_UINT32(BLOCK_NONE, virtualBlockMapPtr->block[0][10].prevBlock);

	/* 4 unmapped bad blocks plus the unmappable BBT block on die 0; the
	 * current block has already been taken from the free list. */
	TEST_ASSERT_EQUAL_UINT32(BBT_BLOCK, phyBlockMapPtr->phyBlock[0][BBT_BLOCK].remappedPhyBlock);
	TEST_ASSERT_EQUAL_UINT32(USER_BLOCKS_PER_DIE - 5 - 1, CountFreeBlocks(0));
	TEST_ASSERT_EQUAL_UINT32(USER_BLOCKS_PER_DIE - 1, CountFreeBlocks(1));
	TEST_ASSERT_EQUAL_UINT32(5 * USER_DIES * MB_PER_BLOCK, mbPerbadBlockSpace);
}

/* ---- RecoverBadBlockTable / FindBadBlock / SaveBadBlockTable ------------- */

static void test_missing_table_is_rebuilt_from_bad_block_marks_and_saved(void)
{
	unsigned int die;

	ftl_test_env_reset_fakes();
	ftl_test_env_preload_bbt_except(DIE0_CH, DIE0_WAY);
	/* Block 5 carries both marks, block 9 only the mark in the last row. */
	fake_nand_mark_bad(DIE0_CH, DIE0_WAY, 5);
	fake_nand_row(DIE0_CH, DIE0_WAY, fake_nand_row_addr(9, BAD_BLOCK_MARK_PAGE1))[BAD_BLOCK_MARK_BYTE1] = 0x00;
	ftl_test_env_bring_up();

	TEST_ASSERT_EQUAL_UINT32(BLOCK_STATE_BAD, phyBlockMapPtr->phyBlock[0][5].bad);
	TEST_ASSERT_EQUAL_UINT32(BLOCK_STATE_BAD, phyBlockMapPtr->phyBlock[0][9].bad);
	TEST_ASSERT_EQUAL_UINT32(BLOCK_STATE_NORMAL, phyBlockMapPtr->phyBlock[0][6].bad);
	TEST_ASSERT_EQUAL_UINT32(BLOCK_STATE_NORMAL, phyBlockMapPtr->phyBlock[0][TOTAL_BLOCKS_PER_DIE - 1].bad);
	TEST_ASSERT_EQUAL_UINT32(LUN0_FIRST_FREE_RESERVED, phyBlockMapPtr->phyBlock[0][5].remappedPhyBlock);
	TEST_ASSERT_EQUAL_UINT32(LUN0_FIRST_FREE_RESERVED + 1, phyBlockMapPtr->phyBlock[0][9].remappedPhyBlock);

	/* The freshly built table was written to NAND in the BBT block of that die only. */
	TEST_ASSERT_EQUAL_UINT8(BLOCK_STATE_BAD, *ftl_test_bbt_entry(DIE0_CH, DIE0_WAY, BBT_BLOCK, 5));
	TEST_ASSERT_EQUAL_UINT8(BLOCK_STATE_BAD, *ftl_test_bbt_entry(DIE0_CH, DIE0_WAY, BBT_BLOCK, 9));
	TEST_ASSERT_EQUAL_UINT8(BLOCK_STATE_NORMAL, *ftl_test_bbt_entry(DIE0_CH, DIE0_WAY, BBT_BLOCK, 6));
	TEST_ASSERT_EQUAL_UINT32(1, fake_nand_block_erase_count(DIE0_CH, DIE0_WAY, BBT_BLOCK));
	TEST_ASSERT_EQUAL_UINT32(USED_PAGES_FOR_BAD_BLOCK_TABLE_PER_DIE, fake_nand_block_program_count(DIE0_CH, DIE0_WAY, BBT_BLOCK));

	for (die = 1; die < USER_DIES; die++)
	{
		TEST_ASSERT_EQUAL_UINT32(0, fake_nand_block_erase_count(Vdie2PchTranslation(die), Vdie2PwayTranslation(die), BBT_BLOCK));
		TEST_ASSERT_EQUAL_UINT32(BLOCK_STATE_NORMAL, phyBlockMapPtr->phyBlock[die][5].bad);
		TEST_ASSERT_EQUAL_UINT32(BBT_INFO_GROWN_BAD_UPDATE_NONE, bbtInfoMapPtr->bbtInfo[die].grownBadUpdate);
	}
}

static void test_rebuilt_table_is_reused_on_next_boot_without_rescanning(void)
{
	FAKE_NAND_DIE_STATS scanBoot, secondBoot;

	ftl_test_env_reset_fakes();
	ftl_test_env_preload_bbt_except(DIE0_CH, DIE0_WAY);
	fake_nand_mark_bad(DIE0_CH, DIE0_WAY, 17);
	ftl_test_env_bring_up();
	scanBoot = fake_nand_stats(DIE0_CH, DIE0_WAY);

	ftl_test_env_bring_up();
	secondBoot = fake_nand_stats(DIE0_CH, DIE0_WAY);
	secondBoot.readTriggers -= scanBoot.readTriggers;
	secondBoot.programs -= scanBoot.programs;

	TEST_ASSERT_EQUAL_UINT32(BLOCK_STATE_BAD, phyBlockMapPtr->phyBlock[0][17].bad);
	TEST_ASSERT_EQUAL_UINT32(1, fake_nand_block_erase_count(DIE0_CH, DIE0_WAY, BBT_BLOCK));
	TEST_ASSERT_EQUAL_UINT32(0, secondBoot.programs);
	/* The scan reads at least one mark page per block; the second boot only reads the table. */
	TEST_ASSERT_GREATER_OR_EQUAL_UINT32(TOTAL_BLOCKS_PER_DIE, scanBoot.readTriggers - secondBoot.readTriggers);
}

/* ---- Grown bad blocks ---------------------------------------------------- */

static void test_grown_bad_block_flush_rewrites_table_of_booked_dies_only(void)
{
	unsigned int die = 3, ch, way, otherCh, otherWay;

	ftl_test_env_init();
	ch = Vdie2PchTranslation(die);
	way = Vdie2PwayTranslation(die);
	otherCh = Vdie2PchTranslation(0);
	otherWay = Vdie2PwayTranslation(0);

	UpdatePhyBlockMapForGrownBadBlock(die, 42);
	TEST_ASSERT_EQUAL_UINT32(BLOCK_STATE_BAD, phyBlockMapPtr->phyBlock[die][42].bad);
	TEST_ASSERT_EQUAL_UINT32(BBT_INFO_GROWN_BAD_UPDATE_BOOKED, bbtInfoMapPtr->bbtInfo[die].grownBadUpdate);
	TEST_ASSERT_EQUAL_UINT8(BLOCK_STATE_NORMAL, *ftl_test_bbt_entry(ch, way, BBT_BLOCK, 42));

	UpdateBadBlockTableForGrownBadBlock(RESERVED_DATA_BUFFER_BASE_ADDR);

	TEST_ASSERT_EQUAL_UINT8(BLOCK_STATE_BAD, *ftl_test_bbt_entry(ch, way, BBT_BLOCK, 42));
	TEST_ASSERT_EQUAL_UINT8(BLOCK_STATE_NORMAL, *ftl_test_bbt_entry(ch, way, BBT_BLOCK, 41));
	/* The table block never marks itself bad. */
	TEST_ASSERT_EQUAL_UINT8(BLOCK_STATE_NORMAL, *ftl_test_bbt_entry(ch, way, BBT_BLOCK, BBT_BLOCK));
	TEST_ASSERT_EQUAL_UINT32(1, fake_nand_block_erase_count(ch, way, BBT_BLOCK));
	TEST_ASSERT_EQUAL_UINT32(USED_PAGES_FOR_BAD_BLOCK_TABLE_PER_DIE, fake_nand_block_program_count(ch, way, BBT_BLOCK));

	TEST_ASSERT_EQUAL_UINT32(0, fake_nand_block_erase_count(otherCh, otherWay, BBT_BLOCK));
	TEST_ASSERT_EQUAL_UINT32(0, ftl_test_pending_nand_reqs());
}

static void test_grown_bad_block_flush_with_nothing_booked_touches_no_nand(void)
{
	FAKE_NAND_DIE_STATS before[USER_DIES];
	unsigned int die;

	ftl_test_env_init();
	for (die = 0; die < USER_DIES; die++)
		before[die] = fake_nand_stats(Vdie2PchTranslation(die), Vdie2PwayTranslation(die));

	UpdateBadBlockTableForGrownBadBlock(RESERVED_DATA_BUFFER_BASE_ADDR);

	for (die = 0; die < USER_DIES; die++)
	{
		FAKE_NAND_DIE_STATS s = fake_nand_stats(Vdie2PchTranslation(die), Vdie2PwayTranslation(die));
		TEST_ASSERT_EQUAL_UINT32(before[die].erases, s.erases);
		TEST_ASSERT_EQUAL_UINT32(before[die].programs, s.programs);
	}
}

static void test_flushed_grown_bad_block_survives_reboot(void)
{
	ftl_test_env_init();
	UpdatePhyBlockMapForGrownBadBlock(0, 77);
	UpdateBadBlockTableForGrownBadBlock(RESERVED_DATA_BUFFER_BASE_ADDR);

	ftl_test_env_bring_up();

	TEST_ASSERT_EQUAL_UINT32(BLOCK_STATE_BAD, phyBlockMapPtr->phyBlock[0][77].bad);
	TEST_ASSERT_EQUAL_UINT32(LUN0_FIRST_FREE_RESERVED, phyBlockMapPtr->phyBlock[0][77].remappedPhyBlock);
	TEST_ASSERT_EQUAL_UINT32(BLOCK_STATE_NORMAL, phyBlockMapPtr->phyBlock[1][77].bad);
}

/* ---- Erase helpers ------------------------------------------------------- */

static void test_erase_total_block_space_erases_every_physical_block_once(void)
{
	unsigned int die, erasesBefore[USER_DIES], bbtErasesBefore[USER_DIES];

	ftl_test_env_init();
	for (die = 0; die < USER_DIES; die++)
	{
		unsigned int ch = Vdie2PchTranslation(die), way = Vdie2PwayTranslation(die);
		erasesBefore[die] = fake_nand_stats(ch, way).erases;
		bbtErasesBefore[die] = fake_nand_block_erase_count(ch, way, BBT_BLOCK);
		/* The last reserved block of LUN1 is never touched by normal bring-up. */
		TEST_ASSERT_EQUAL_UINT32(0, fake_nand_block_erase_count(ch, way, TOTAL_BLOCKS_PER_DIE - 1));
	}

	EraseTotalBlockSpace();

	for (die = 0; die < USER_DIES; die++)
	{
		unsigned int ch = Vdie2PchTranslation(die), way = Vdie2PwayTranslation(die);
		TEST_ASSERT_EQUAL_UINT32(TOTAL_BLOCKS_PER_DIE, fake_nand_stats(ch, way).erases - erasesBefore[die]);
		TEST_ASSERT_EQUAL_UINT32(bbtErasesBefore[die] + 1, fake_nand_block_erase_count(ch, way, BBT_BLOCK));
		TEST_ASSERT_EQUAL_UINT32(1, fake_nand_block_erase_count(ch, way, TOTAL_BLOCKS_PER_DIE - 1));
	}
	TEST_ASSERT_EQUAL_UINT32(0, ftl_test_pending_nand_reqs());
}

int main(void)
{
	UNITY_BEGIN();
	RUN_TEST(test_lun1_bad_block_is_remapped_onto_lun1_reserved_area);
	RUN_TEST(test_remap_skips_reserved_blocks_that_are_themselves_bad);
	RUN_TEST(test_bad_blocks_without_reserved_spares_stay_bad_and_shrink_capacity);
	RUN_TEST(test_missing_table_is_rebuilt_from_bad_block_marks_and_saved);
	RUN_TEST(test_rebuilt_table_is_reused_on_next_boot_without_rescanning);
	RUN_TEST(test_grown_bad_block_flush_rewrites_table_of_booked_dies_only);
	RUN_TEST(test_grown_bad_block_flush_with_nothing_booked_touches_no_nand);
	RUN_TEST(test_flushed_grown_bad_block_survives_reboot);
	RUN_TEST(test_erase_total_block_space_erases_every_physical_block_once);
	return UNITY_END();
}
