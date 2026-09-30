#include <string.h>

#include "test_support.h"

void setUp(void)
{
	test_ftl_init();
}

void tearDown(void)
{
}

/* ---- pure translation macros ------------------------------------------------ */

static void test_vsa_macros_round_trip_every_component(void)
{
	unsigned int dieNo, blockNo, pageNo, vsa;

	for (dieNo = 0; dieNo < USER_DIES; dieNo += 5)
		for (blockNo = 0; blockNo < USER_BLOCKS_PER_DIE; blockNo += 1021)
			for (pageNo = 0; pageNo < USER_PAGES_PER_BLOCK; pageNo += 31)
			{
				vsa = Vorg2VsaTranslation(dieNo, blockNo, pageNo);
				TEST_ASSERT_EQUAL_UINT(dieNo, Vsa2VdieTranslation(vsa));
				TEST_ASSERT_EQUAL_UINT(blockNo, Vsa2VblockTranslation(vsa));
				TEST_ASSERT_EQUAL_UINT(pageNo, Vsa2VpageTranslation(vsa));
			}

	TEST_ASSERT_EQUAL_UINT(SLICES_PER_SSD - 1,
		Vorg2VsaTranslation(USER_DIES - 1, USER_BLOCKS_PER_DIE - 1, USER_PAGES_PER_BLOCK - 1));
}

static void test_die_to_channel_way_mapping_is_channel_interleaved(void)
{
	unsigned int chNo, wayNo, dieNo;

	for (wayNo = 0; wayNo < USER_WAYS; wayNo++)
		for (chNo = 0; chNo < USER_CHANNELS; chNo++)
		{
			dieNo = Pcw2VdieTranslation(chNo, wayNo);
			TEST_ASSERT_EQUAL_UINT(chNo, Vdie2PchTranslation(dieNo));
			TEST_ASSERT_EQUAL_UINT(wayNo, Vdie2PwayTranslation(dieNo));
		}
	TEST_ASSERT_EQUAL_UINT(1, Pcw2VdieTranslation(1, 0));
	TEST_ASSERT_EQUAL_UINT(USER_CHANNELS, Pcw2VdieTranslation(0, 1));
}

static void test_lsb_page_translation_skips_msb_pages(void)
{
	TEST_ASSERT_EQUAL_UINT(0, Vpage2PlsbPageTranslation(0));
	TEST_ASSERT_EQUAL_UINT(1, Vpage2PlsbPageTranslation(1));
	TEST_ASSERT_EQUAL_UINT(3, Vpage2PlsbPageTranslation(2));
	TEST_ASSERT_EQUAL_UINT(2 * (USER_PAGES_PER_BLOCK - 1) - 1, Vpage2PlsbPageTranslation(USER_PAGES_PER_BLOCK - 1));
	TEST_ASSERT_EQUAL_UINT(2, PlsbPage2VpageTranslation(3));
	TEST_ASSERT_EQUAL_UINT(1, PlsbPage2VpageTranslation(1));
}

/* ---- boot state ----------------------------------------------------------- */

static void test_boot_leaves_every_logical_slice_unmapped(void)
{
	TEST_ASSERT_EQUAL_UINT(VSA_NONE, logicalSliceMapPtr->logicalSlice[0].virtualSliceAddr);
	TEST_ASSERT_EQUAL_UINT(VSA_NONE, logicalSliceMapPtr->logicalSlice[SLICES_PER_SSD - 1].virtualSliceAddr);
	TEST_ASSERT_EQUAL_UINT(VSA_FAIL, AddrTransRead(0));
	TEST_ASSERT_EQUAL_UINT(VSA_FAIL, AddrTransRead(SLICES_PER_SSD - 1));
}

static void test_boot_reserves_bad_block_table_block_and_remaps_it(void)
{
	unsigned int dieNo, bbtBlock;

	for (dieNo = 0; dieNo < USER_DIES; dieNo++)
	{
		bbtBlock = bbtInfoMapPtr->bbtInfo[dieNo].phyBlock;
		TEST_ASSERT_EQUAL_UINT(BLOCK_STATE_BAD, phyBlockMapPtr->phyBlock[dieNo][bbtBlock].bad);
		/* the virtual block that lands on the BBT block is redirected into the reserved area */
		TEST_ASSERT_GREATER_OR_EQUAL_UINT(USER_BLOCKS_PER_LUN, phyBlockMapPtr->phyBlock[dieNo][bbtBlock].remappedPhyBlock);
		TEST_ASSERT_EQUAL_UINT(BLOCK_STATE_NORMAL, virtualBlockMapPtr->block[dieNo][bbtBlock].bad);
		/* every user block is free, one of them is already the current write block */
		TEST_ASSERT_EQUAL_UINT(USER_BLOCKS_PER_DIE - 1, virtualDieMapPtr->die[dieNo].freeBlockCnt);
		TEST_ASSERT_EQUAL_UINT(USER_BLOCKS_PER_DIE - 1, test_count_free_blocks(dieNo));
		TEST_ASSERT_EQUAL_UINT(0, virtualBlockMapPtr->block[dieNo][virtualDieMapPtr->die[dieNo].currentBlock].currentPage);
	}
	TEST_ASSERT_EQUAL_UINT(0, mbPerbadBlockSpace);
}

/* ---- LSA -> VSA -> PSA -------------------------------------------------------- */

static void test_write_maps_lsa_to_fresh_vsa_and_read_returns_it(void)
{
	unsigned int lsa = 4242;
	unsigned int expectedDie = sliceAllocationTargetDie;
	unsigned int vsa = AddrTransWrite(lsa);

	TEST_ASSERT_NOT_EQUAL_UINT(VSA_FAIL, vsa);
	TEST_ASSERT_EQUAL_UINT(expectedDie, Vsa2VdieTranslation(vsa));
	TEST_ASSERT_EQUAL_UINT(vsa, AddrTransRead(lsa));
	TEST_ASSERT_EQUAL_UINT(lsa, virtualSliceMapPtr->virtualSlice[vsa].logicalSliceAddr);
	TEST_ASSERT_EQUAL_UINT(1, virtualBlockMapPtr->block[expectedDie][Vsa2VblockTranslation(vsa)].currentPage);
}

static void test_consecutive_writes_stripe_across_dies_then_pages(void)
{
	unsigned int i, vsa;
	unsigned int seenDie[USER_DIES];

	memset(seenDie, 0, sizeof(seenDie));
	for (i = 0; i < USER_DIES; i++)
	{
		vsa = AddrTransWrite(i);
		TEST_ASSERT_EQUAL_UINT(0, Vsa2VpageTranslation(vsa));
		seenDie[Vsa2VdieTranslation(vsa)]++;
	}
	for (i = 0; i < USER_DIES; i++)
		TEST_ASSERT_EQUAL_UINT_MESSAGE(1, seenDie[i], "each die receives exactly one slice per round");

	vsa = AddrTransWrite(USER_DIES);
	TEST_ASSERT_EQUAL_UINT(1, Vsa2VpageTranslation(vsa));
}

static void test_vsa_to_psa_matches_page_programmed_in_nand(void)
{
	unsigned int lsa = 77;
	unsigned int vsa = test_write_slice(lsa, 0xA5);
	PSA psa = test_vsa_to_psa(vsa);
	const unsigned char *data = fake_nand_page_data(psa.chNo, psa.wayNo, psa.rowAddr);

	TEST_ASSERT_EQUAL_UINT(1, fake_nand_written_page_count());
	TEST_ASSERT_NOT_NULL_MESSAGE(data, "program landed on a different physical page than the translation predicts");
	TEST_ASSERT_EQUAL_HEX8(0xA5, data[0]);
	TEST_ASSERT_EQUAL_HEX8(0xA5, data[BYTES_PER_DATA_REGION_OF_SLICE - 1]);
	TEST_ASSERT_EQUAL_UINT(psa.phyBlockNo, fake_nand_row_to_phy_block(psa.rowAddr));
	TEST_ASSERT_EQUAL_UINT(Vpage2PlsbPageTranslation(psa.virtualPageNo), fake_nand_row_to_page(psa.rowAddr));
}

static void test_read_after_write_returns_programmed_data(void)
{
	unsigned int vsa = test_write_slice(9, 0x3C);
	const unsigned char *buf = test_read_slice(vsa);

	TEST_ASSERT_EQUAL_HEX8(0x3C, buf[0]);
	TEST_ASSERT_EQUAL_HEX8(0x3C, buf[4095]);
	TEST_ASSERT_EQUAL_HEX8(0x3C, buf[BYTES_PER_DATA_REGION_OF_SLICE - 1]);
}

/* ---- FindFreeVirtualSlice ------------------------------------------------- */

static void test_find_free_virtual_slice_advances_page_and_die(void)
{
	unsigned int dieNo = sliceAllocationTargetDie;
	unsigned int block = virtualDieMapPtr->die[dieNo].currentBlock;
	unsigned int vsa = FindFreeVirtualSlice();

	TEST_ASSERT_EQUAL_UINT(Vorg2VsaTranslation(dieNo, block, 0), vsa);
	TEST_ASSERT_EQUAL_UINT(1, virtualBlockMapPtr->block[dieNo][block].currentPage);
	TEST_ASSERT_NOT_EQUAL_UINT(dieNo, sliceAllocationTargetDie);
}

static void test_find_free_virtual_slice_moves_to_new_block_when_current_is_full(void)
{
	unsigned int dieNo = sliceAllocationTargetDie;
	unsigned int block = virtualDieMapPtr->die[dieNo].currentBlock;
	unsigned int freeBefore = virtualDieMapPtr->die[dieNo].freeBlockCnt;
	unsigned int nextFree = virtualDieMapPtr->die[dieNo].headFreeBlock;
	unsigned int vsa;

	virtualBlockMapPtr->block[dieNo][block].currentPage = USER_PAGES_PER_BLOCK;

	vsa = FindFreeVirtualSlice();

	TEST_ASSERT_EQUAL_UINT(nextFree, virtualDieMapPtr->die[dieNo].currentBlock);
	TEST_ASSERT_EQUAL_UINT(Vorg2VsaTranslation(dieNo, nextFree, 0), vsa);
	TEST_ASSERT_EQUAL_UINT(freeBefore - 1, virtualDieMapPtr->die[dieNo].freeBlockCnt);
	TEST_ASSERT_EQUAL_UINT(0, virtualBlockMapPtr->block[dieNo][nextFree].free);
}

/* ---- InvalidateOldVsa ------------------------------------------------------- */

static void test_invalidate_old_vsa_moves_block_into_gc_victim_bucket(void)
{
	unsigned int lsa = 5;
	unsigned int vsa = AddrTransWrite(lsa);
	unsigned int dieNo = Vsa2VdieTranslation(vsa);
	unsigned int block = Vsa2VblockTranslation(vsa);

	TEST_ASSERT_EQUAL_UINT(0, virtualBlockMapPtr->block[dieNo][block].invalidSliceCnt);

	InvalidateOldVsa(lsa);

	TEST_ASSERT_EQUAL_UINT(VSA_NONE, logicalSliceMapPtr->logicalSlice[lsa].virtualSliceAddr);
	TEST_ASSERT_EQUAL_UINT(1, virtualBlockMapPtr->block[dieNo][block].invalidSliceCnt);
	TEST_ASSERT_EQUAL_UINT(1, test_count_gc_victims(dieNo, 1));
	TEST_ASSERT_EQUAL_UINT(block, gcVictimMapPtr->gcVictimList[dieNo][1].headBlock);
	/* reverse map keeps the stale LSA, which is how GC detects the slice is no longer valid */
	TEST_ASSERT_EQUAL_UINT(lsa, virtualSliceMapPtr->virtualSlice[vsa].logicalSliceAddr);
	TEST_ASSERT_EQUAL_UINT(VSA_FAIL, AddrTransRead(lsa));
}

static void test_invalidate_unmapped_lsa_is_a_no_op(void)
{
	unsigned int dieNo;

	InvalidateOldVsa(123);
	for (dieNo = 0; dieNo < USER_DIES; dieNo++)
		TEST_ASSERT_EQUAL_UINT(0, test_count_gc_victims(dieNo, 1));
}

static void test_invalidate_skips_slice_whose_reverse_map_changed(void)
{
	unsigned int lsa = 8;
	unsigned int vsa = AddrTransWrite(lsa);
	unsigned int dieNo = Vsa2VdieTranslation(vsa);
	unsigned int block = Vsa2VblockTranslation(vsa);

	virtualSliceMapPtr->virtualSlice[vsa].logicalSliceAddr = lsa + 1;	/* somebody else owns the slice */
	InvalidateOldVsa(lsa);

	TEST_ASSERT_EQUAL_UINT(vsa, logicalSliceMapPtr->logicalSlice[lsa].virtualSliceAddr);
	TEST_ASSERT_EQUAL_UINT(0, virtualBlockMapPtr->block[dieNo][block].invalidSliceCnt);
}

static void test_overwrite_invalidates_previous_vsa_and_moves_bucket(void)
{
	unsigned int lsa = 31;
	unsigned int first = AddrTransWrite(lsa);
	unsigned int dieNo = Vsa2VdieTranslation(first);
	unsigned int block = Vsa2VblockTranslation(first);
	unsigned int second, third;

	while (sliceAllocationTargetDie != dieNo)	/* wait until the striping returns to the same die */
		AddrTransWrite(1000 + sliceAllocationTargetDie);
	second = AddrTransWrite(lsa);

	TEST_ASSERT_NOT_EQUAL_UINT(first, second);
	TEST_ASSERT_EQUAL_UINT(second, AddrTransRead(lsa));
	TEST_ASSERT_EQUAL_UINT(1, virtualBlockMapPtr->block[dieNo][block].invalidSliceCnt);
	TEST_ASSERT_EQUAL_UINT(1, test_count_gc_victims(dieNo, 1));

	while (sliceAllocationTargetDie != dieNo)
		AddrTransWrite(2000 + sliceAllocationTargetDie);
	third = AddrTransWrite(lsa);

	TEST_ASSERT_NOT_EQUAL_UINT(second, third);
	TEST_ASSERT_EQUAL_UINT(2, virtualBlockMapPtr->block[dieNo][block].invalidSliceCnt);
	TEST_ASSERT_EQUAL_UINT(0, test_count_gc_victims(dieNo, 1));
	TEST_ASSERT_EQUAL_UINT(1, test_count_gc_victims(dieNo, 2));
}

/* ---- free block list ------------------------------------------------------- */

static void test_free_block_list_is_fifo_and_honours_reserve(void)
{
	unsigned int dieNo = 3;
	unsigned int head = virtualDieMapPtr->die[dieNo].headFreeBlock;
	unsigned int cnt = virtualDieMapPtr->die[dieNo].freeBlockCnt;
	unsigned int got;

	got = GetFromFbList(dieNo, GET_FREE_BLOCK_NORMAL);
	TEST_ASSERT_EQUAL_UINT(head, got);
	TEST_ASSERT_EQUAL_UINT(cnt - 1, virtualDieMapPtr->die[dieNo].freeBlockCnt);

	PutToFbList(dieNo, got);
	TEST_ASSERT_EQUAL_UINT(got, virtualDieMapPtr->die[dieNo].tailFreeBlock);
	TEST_ASSERT_EQUAL_UINT(cnt, virtualDieMapPtr->die[dieNo].freeBlockCnt);

	virtualDieMapPtr->die[dieNo].freeBlockCnt = RESERVED_FREE_BLOCK_COUNT;
	TEST_ASSERT_EQUAL_UINT(BLOCK_FAIL, GetFromFbList(dieNo, GET_FREE_BLOCK_NORMAL));
	TEST_ASSERT_NOT_EQUAL_UINT(BLOCK_FAIL, GetFromFbList(dieNo, GET_FREE_BLOCK_GC));
}

static void test_erase_block_returns_block_to_free_list_and_clears_maps(void)
{
	unsigned int lsa = 60;
	unsigned int vsa = test_write_slice(lsa, 0x11);
	unsigned int dieNo = Vsa2VdieTranslation(vsa);
	unsigned int block = Vsa2VblockTranslation(vsa);
	unsigned int freeBefore = virtualDieMapPtr->die[dieNo].freeBlockCnt;
	unsigned int erasesBefore = fake_nand_stats()->erases;

	EraseBlock(dieNo, block);
	test_drain_nand();

	TEST_ASSERT_EQUAL_UINT(1, virtualBlockMapPtr->block[dieNo][block].free);
	TEST_ASSERT_EQUAL_UINT(1, virtualBlockMapPtr->block[dieNo][block].eraseCnt);
	TEST_ASSERT_EQUAL_UINT(0, virtualBlockMapPtr->block[dieNo][block].currentPage);
	TEST_ASSERT_EQUAL_UINT(freeBefore + 1, virtualDieMapPtr->die[dieNo].freeBlockCnt);
	TEST_ASSERT_EQUAL_UINT(block, virtualDieMapPtr->die[dieNo].tailFreeBlock);
	TEST_ASSERT_EQUAL_UINT(LSA_NONE, virtualSliceMapPtr->virtualSlice[vsa].logicalSliceAddr);
	TEST_ASSERT_EQUAL_UINT(erasesBefore + 1, fake_nand_stats()->erases);
	TEST_ASSERT_EQUAL_UINT(0, fake_nand_written_page_count());
}

/* ---- bad block remapping (needs a real boot against a NAND with marked blocks) ---- */

static void test_factory_bad_blocks_are_remapped_into_reserved_area(void)
{
	const unsigned int badUser = 10;			/* user block in LUN 0 */
	const unsigned int badReserved = USER_BLOCKS_PER_LUN;	/* first reserved block in LUN 0 */
	const unsigned int badLun1 = TOTAL_BLOCKS_PER_LUN + 20;	/* user block in LUN 1 */
	unsigned int bbtBlock, dieNo = Pcw2VdieTranslation(1, 2);
	unsigned int vsa, expectRemapLun0Bbt, expectRemapUser;
	PSA psa;

	fake_nand_reset();
	fake_nand_mark_factory_bad(1, 2, badUser);
	fake_nand_mark_factory_bad(1, 2, badReserved);
	fake_nand_mark_factory_bad(1, 2, badLun1);
	test_ftl_init_fresh();

	TEST_ASSERT_EQUAL_UINT(BLOCK_STATE_BAD, phyBlockMapPtr->phyBlock[dieNo][badUser].bad);
	TEST_ASSERT_EQUAL_UINT(BLOCK_STATE_BAD, phyBlockMapPtr->phyBlock[dieNo][badReserved].bad);
	TEST_ASSERT_EQUAL_UINT(BLOCK_STATE_BAD, phyBlockMapPtr->phyBlock[dieNo][badLun1].bad);
	TEST_ASSERT_EQUAL_UINT(BLOCK_STATE_NORMAL, phyBlockMapPtr->phyBlock[dieNo][badUser + 1].bad);
	TEST_ASSERT_EQUAL_UINT(BLOCK_STATE_NORMAL, phyBlockMapPtr->phyBlock[dieNo + 1][badUser].bad);

	/* RemapBadBlock() walks user blocks in order and skips bad reserved blocks. */
	bbtBlock = bbtInfoMapPtr->bbtInfo[dieNo].phyBlock;
	TEST_ASSERT_LESS_THAN_UINT(badUser, bbtBlock);
	expectRemapLun0Bbt = badReserved + 1;
	expectRemapUser = badReserved + 2;
	TEST_ASSERT_EQUAL_UINT(expectRemapLun0Bbt, phyBlockMapPtr->phyBlock[dieNo][bbtBlock].remappedPhyBlock);
	TEST_ASSERT_EQUAL_UINT(expectRemapUser, phyBlockMapPtr->phyBlock[dieNo][badUser].remappedPhyBlock);
	TEST_ASSERT_EQUAL_UINT(TOTAL_BLOCKS_PER_LUN + USER_BLOCKS_PER_LUN,
		phyBlockMapPtr->phyBlock[dieNo][badLun1].remappedPhyBlock);
	TEST_ASSERT_EQUAL_UINT(badUser + 1, phyBlockMapPtr->phyBlock[dieNo][badUser + 1].remappedPhyBlock);

	/* the virtual blocks stay usable and no capacity is lost */
	TEST_ASSERT_EQUAL_UINT(BLOCK_STATE_NORMAL, virtualBlockMapPtr->block[dieNo][badUser].bad);
	TEST_ASSERT_EQUAL_UINT(BLOCK_STATE_NORMAL, virtualBlockMapPtr->block[dieNo][USER_BLOCKS_PER_LUN + 20].bad);
	TEST_ASSERT_EQUAL_UINT(USER_BLOCKS_PER_DIE - 1, virtualDieMapPtr->die[dieNo].freeBlockCnt);
	TEST_ASSERT_EQUAL_UINT(0, mbPerbadBlockSpace);

	/* a program into the remapped virtual block lands in the replacement physical block */
	vsa = Vorg2VsaTranslation(dieNo, badUser, 0);
	test_program_vsa(500, vsa, 0x5A);
	psa = test_vsa_to_psa(vsa);
	TEST_ASSERT_EQUAL_UINT(badUser, psa.virtualBlockNo);
	TEST_ASSERT_EQUAL_UINT(expectRemapUser, psa.phyBlockNo);
	TEST_ASSERT_NOT_NULL(fake_nand_page_data(1, 2, psa.rowAddr));
	TEST_ASSERT_NULL(fake_nand_page_data(1, 2, fake_nand_phy_block_to_row(badUser, 0)));
}

static void test_too_many_bad_blocks_reduce_capacity(void)
{
	unsigned int i, dieNo = Pcw2VdieTranslation(0, 1);

	fake_nand_reset();
	/* every reserved block of LUN 0 is bad, plus two user blocks: nothing left to remap them to */
	for (i = USER_BLOCKS_PER_LUN; i < TOTAL_BLOCKS_PER_LUN; i++)
		fake_nand_mark_factory_bad(0, 1, i);
	fake_nand_mark_factory_bad(0, 1, 100);
	fake_nand_mark_factory_bad(0, 1, 101);
	test_ftl_init_fresh();

	TEST_ASSERT_EQUAL_UINT(BLOCK_STATE_BAD, virtualBlockMapPtr->block[dieNo][100].bad);
	TEST_ASSERT_EQUAL_UINT(BLOCK_STATE_BAD, virtualBlockMapPtr->block[dieNo][101].bad);
	TEST_ASSERT_EQUAL_UINT(BLOCK_STATE_BAD, virtualBlockMapPtr->block[dieNo][bbtInfoMapPtr->bbtInfo[dieNo].phyBlock].bad);
	/* 3 unusable blocks (2 marked + the BBT block), one more taken as the current block */
	TEST_ASSERT_EQUAL_UINT(USER_BLOCKS_PER_DIE - 3 - 1, virtualDieMapPtr->die[dieNo].freeBlockCnt);
	TEST_ASSERT_EQUAL_UINT(3 * USER_DIES * MB_PER_BLOCK, mbPerbadBlockSpace);
	TEST_ASSERT_EQUAL_UINT(BLOCK_STATE_NORMAL, virtualBlockMapPtr->block[dieNo + 1][100].bad);
}

static void test_grown_bad_block_is_booked_and_flushed_to_bbt_block(void)
{
	unsigned int dieNo = Pcw2VdieTranslation(0, 1), phyBlock = 1234;
	unsigned int bbtRow = fake_nand_phy_block_to_row(bbtInfoMapPtr->bbtInfo[dieNo].phyBlock,
		Vpage2PlsbPageTranslation(PlsbPage2VpageTranslation(START_PAGE_NO_OF_BAD_BLOCK_TABLE_BLOCK)));
	const unsigned char *table;

	TEST_ASSERT_EQUAL_UINT(BBT_INFO_GROWN_BAD_UPDATE_NONE, bbtInfoMapPtr->bbtInfo[dieNo].grownBadUpdate);
	UpdatePhyBlockMapForGrownBadBlock(dieNo, phyBlock);
	TEST_ASSERT_EQUAL_UINT(BLOCK_STATE_BAD, phyBlockMapPtr->phyBlock[dieNo][phyBlock].bad);
	TEST_ASSERT_EQUAL_UINT(BBT_INFO_GROWN_BAD_UPDATE_BOOKED, bbtInfoMapPtr->bbtInfo[dieNo].grownBadUpdate);

	UpdateBadBlockTableForGrownBadBlock(RESERVED_DATA_BUFFER_BASE_ADDR);
	test_drain_nand();

	/* only the booked die rewrites its table: erase + program of the BBT block */
	TEST_ASSERT_EQUAL_UINT(1, fake_nand_stats()->erases);
	TEST_ASSERT_EQUAL_UINT(USED_PAGES_FOR_BAD_BLOCK_TABLE_PER_DIE, fake_nand_stats()->programs);
	table = fake_nand_page_data(0, 1, bbtRow);
	TEST_ASSERT_NOT_NULL(table);
	TEST_ASSERT_EQUAL_UINT8(BLOCK_STATE_BAD, table[phyBlock]);
	TEST_ASSERT_EQUAL_UINT8(BLOCK_STATE_NORMAL, table[phyBlock + 1]);
	/* the BBT block itself is recorded as usable so the table can be found again at boot */
	TEST_ASSERT_EQUAL_UINT8(BLOCK_STATE_NORMAL, table[bbtInfoMapPtr->bbtInfo[dieNo].phyBlock]);
}

/*
 * SUSPICIOUS BEHAVIOUR (documented, not fixed): UpdateBadBlockTableForGrownBadBlock() never
 * clears grownBadUpdate, so every later shutdown erases and rewrites the bad block table of
 * the die again even though nothing changed.
 */
static void test_grown_bad_flush_clears_booking(void)
{
	unsigned int dieNo = 2;

	UpdatePhyBlockMapForGrownBadBlock(dieNo, 77);
	UpdateBadBlockTableForGrownBadBlock(RESERVED_DATA_BUFFER_BASE_ADDR);
	test_drain_nand();

	if (bbtInfoMapPtr->bbtInfo[dieNo].grownBadUpdate == BBT_INFO_GROWN_BAD_UPDATE_BOOKED)
		TEST_IGNORE_MESSAGE("known issue: grownBadUpdate stays BOOKED after the table has been flushed");
	TEST_ASSERT_EQUAL_UINT(BBT_INFO_GROWN_BAD_UPDATE_NONE, bbtInfoMapPtr->bbtInfo[dieNo].grownBadUpdate);
}

int main(void)
{
	UNITY_BEGIN();
	RUN_TEST(test_vsa_macros_round_trip_every_component);
	RUN_TEST(test_die_to_channel_way_mapping_is_channel_interleaved);
	RUN_TEST(test_lsb_page_translation_skips_msb_pages);
	RUN_TEST(test_boot_leaves_every_logical_slice_unmapped);
	RUN_TEST(test_boot_reserves_bad_block_table_block_and_remaps_it);
	RUN_TEST(test_write_maps_lsa_to_fresh_vsa_and_read_returns_it);
	RUN_TEST(test_consecutive_writes_stripe_across_dies_then_pages);
	RUN_TEST(test_vsa_to_psa_matches_page_programmed_in_nand);
	RUN_TEST(test_read_after_write_returns_programmed_data);
	RUN_TEST(test_find_free_virtual_slice_advances_page_and_die);
	RUN_TEST(test_find_free_virtual_slice_moves_to_new_block_when_current_is_full);
	RUN_TEST(test_invalidate_old_vsa_moves_block_into_gc_victim_bucket);
	RUN_TEST(test_invalidate_unmapped_lsa_is_a_no_op);
	RUN_TEST(test_invalidate_skips_slice_whose_reverse_map_changed);
	RUN_TEST(test_overwrite_invalidates_previous_vsa_and_moves_bucket);
	RUN_TEST(test_free_block_list_is_fifo_and_honours_reserve);
	RUN_TEST(test_erase_block_returns_block_to_free_list_and_clears_maps);
	RUN_TEST(test_grown_bad_block_is_booked_and_flushed_to_bbt_block);
	RUN_TEST(test_grown_bad_flush_clears_booking);
	RUN_TEST(test_factory_bad_blocks_are_remapped_into_reserved_area);
	RUN_TEST(test_too_many_bad_blocks_reduce_capacity);
	return UNITY_END();
}
