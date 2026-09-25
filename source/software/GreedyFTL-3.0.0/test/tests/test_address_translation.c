#include "test_support.h"

void setUp(void) { TestFtlReset(); }
void tearDown(void) {}

/* Builds a VSA-addressed NAND request so GenerateNandRowAddr() can be exercised. */
static unsigned int MakeVsaReq(unsigned int vsa)
{
	unsigned int tag = GetFromFreeReqQ();
	reqPoolPtr->reqPool[tag].reqType = REQ_TYPE_NAND;
	reqPoolPtr->reqPool[tag].reqCode = REQ_CODE_READ;
	reqPoolPtr->reqPool[tag].reqOpt.nandAddr = REQ_OPT_NAND_ADDR_VSA;
	reqPoolPtr->reqPool[tag].reqOpt.blockSpace = REQ_OPT_BLOCK_SPACE_MAIN;
	reqPoolPtr->reqPool[tag].nandInfo.virtualSliceAddr = vsa;
	return tag;
}

/* ---------------------------------------------------------------- LBA -> VSA -> PSA */

void test_unwritten_lsa_reads_as_vsa_fail(void)
{
	TEST_ASSERT_EQUAL_HEX32(VSA_FAIL, AddrTransRead(0));
	TEST_ASSERT_EQUAL_HEX32(VSA_FAIL, AddrTransRead(SLICES_PER_SSD - 1));
}

void test_write_then_read_round_trips_through_both_maps(void)
{
	unsigned int lsa = 4242;
	unsigned int vsa = AddrTransWrite(lsa);

	TEST_ASSERT_NOT_EQUAL(VSA_FAIL, vsa);
	TEST_ASSERT_EQUAL_UINT(vsa, AddrTransRead(lsa));
	TEST_ASSERT_EQUAL_UINT(vsa, logicalSliceMapPtr->logicalSlice[lsa].virtualSliceAddr);
	TEST_ASSERT_EQUAL_UINT(lsa, virtualSliceMapPtr->virtualSlice[vsa].logicalSliceAddr);
}

void test_vsa_decomposes_into_die_block_page_and_back(void)
{
	unsigned int vsa = AddrTransWrite(7);
	unsigned int die = TestDieOf(vsa);
	unsigned int block = TestBlockOf(vsa);
	unsigned int page = TestPageOf(vsa);

	TEST_ASSERT_TRUE(die < USER_DIES);
	TEST_ASSERT_TRUE(block < USER_BLOCKS_PER_DIE);
	TEST_ASSERT_TRUE(page < USER_PAGES_PER_BLOCK);
	TEST_ASSERT_EQUAL_UINT(vsa, Vorg2VsaTranslation(die, block, page));
	TEST_ASSERT_EQUAL_UINT(block, virtualDieMapPtr->die[die].currentBlock);
}

void test_vsa_maps_to_physical_row_address(void)
{
	unsigned int vsa = AddrTransWrite(0);
	unsigned int die = TestDieOf(vsa);
	unsigned int vblock = TestBlockOf(vsa);
	unsigned int page = TestPageOf(vsa);
	unsigned int pblock = Vblock2PblockOfTbsTranslation(vblock);
	unsigned int lun = pblock / TOTAL_BLOCKS_PER_LUN;
	unsigned int expectedRow = (lun ? LUN_1_BASE_ADDR : LUN_0_BASE_ADDR)
		+ (phyBlockMapPtr->phyBlock[die][pblock].remappedPhyBlock % TOTAL_BLOCKS_PER_LUN) * PAGES_PER_MLC_BLOCK
		+ Vpage2PlsbPageTranslation(page);
	unsigned int tag = MakeVsaReq(vsa);

	TEST_ASSERT_EQUAL_HEX32(expectedRow, GenerateNandRowAddr(tag));
	/* No remapping on a clean device: physical == virtual block within the LUN. */
	TEST_ASSERT_EQUAL_UINT(pblock, phyBlockMapPtr->phyBlock[die][pblock].remappedPhyBlock);
	/* And the fake NAND agrees on how to decode the row it will be handed. */
	TEST_ASSERT_EQUAL_UINT(pblock, FakeNandRowToPhyBlock(expectedRow));
	TEST_ASSERT_EQUAL_UINT(Vpage2PlsbPageTranslation(page), FakeNandRowToPage(expectedRow));
}

void test_second_lun_vsa_uses_lun1_row_base(void)
{
	unsigned int die = 0;
	unsigned int vblock = USER_BLOCKS_PER_LUN; /* first block of LUN 1 */
	unsigned int vsa = Vorg2VsaTranslation(die, vblock, 3);
	unsigned int tag = MakeVsaReq(vsa);

	/* SLC mode only programs LSB pages: virtual page n lands on physical row 2n-1. */
	TEST_ASSERT_EQUAL_HEX32(LUN_1_BASE_ADDR + Vpage2PlsbPageTranslation(3), GenerateNandRowAddr(tag));
}

/* ---------------------------------------------------------------- FindFreeVirtualSlice */

void test_find_free_virtual_slice_round_robins_across_dies(void)
{
	unsigned int i;
	for (i = 0; i < USER_DIES * 2; i++)
	{
		unsigned int vsa = FindFreeVirtualSlice();
		TEST_ASSERT_EQUAL_UINT(i % USER_DIES, TestDieOf(vsa));
		TEST_ASSERT_EQUAL_UINT(i / USER_DIES, TestPageOf(vsa));
	}
}

void test_find_free_virtual_slice_advances_current_page(void)
{
	unsigned int block = virtualDieMapPtr->die[0].currentBlock;
	unsigned int vsa;

	TEST_ASSERT_EQUAL_UINT(0, virtualBlockMapPtr->block[0][block].currentPage);
	vsa = FindFreeVirtualSlice();
	TEST_ASSERT_EQUAL_UINT(0, TestDieOf(vsa));
	TEST_ASSERT_EQUAL_UINT(1, virtualBlockMapPtr->block[0][block].currentPage);
	TEST_ASSERT_EQUAL_UINT(0, virtualBlockMapPtr->block[0][block].free);
}

void test_find_free_virtual_slice_moves_to_next_free_block_when_full(void)
{
	unsigned int firstBlock = virtualDieMapPtr->die[0].currentBlock;
	unsigned int freeBefore = virtualDieMapPtr->die[0].freeBlockCnt;
	unsigned int i, vsa;

	for (i = 0; i < USER_DIES * USER_PAGES_PER_BLOCK; i++)
		FindFreeVirtualSlice();

	TEST_ASSERT_EQUAL_UINT(USER_PAGES_PER_BLOCK, virtualBlockMapPtr->block[0][firstBlock].currentPage);
	TEST_ASSERT_EQUAL_UINT(firstBlock, virtualDieMapPtr->die[0].currentBlock);

	vsa = FindFreeVirtualSlice();
	TEST_ASSERT_EQUAL_UINT(0, TestDieOf(vsa));
	TEST_ASSERT_NOT_EQUAL(firstBlock, TestBlockOf(vsa));
	TEST_ASSERT_EQUAL_UINT(0, TestPageOf(vsa));
	TEST_ASSERT_EQUAL_UINT(TestBlockOf(vsa), virtualDieMapPtr->die[0].currentBlock);
	TEST_ASSERT_EQUAL_UINT(freeBefore - 1, virtualDieMapPtr->die[0].freeBlockCnt);
}

/* ---------------------------------------------------------------- free block list */

void test_free_block_list_holds_all_but_current_block_after_init(void)
{
	unsigned int die;
	for (die = 0; die < USER_DIES; die++)
	{
		TEST_ASSERT_EQUAL_UINT(USER_BLOCKS_PER_DIE - 1, virtualDieMapPtr->die[die].freeBlockCnt);
		TEST_ASSERT_NOT_EQUAL(BLOCK_NONE, virtualDieMapPtr->die[die].headFreeBlock);
		TEST_ASSERT_NOT_EQUAL(BLOCK_NONE, virtualDieMapPtr->die[die].tailFreeBlock);
	}
}

void test_get_from_fb_list_normal_keeps_reserved_blocks_for_gc(void)
{
	unsigned int taken = 0;
	while (GetFromFbList(0, GET_FREE_BLOCK_NORMAL) != BLOCK_FAIL)
		taken++;

	TEST_ASSERT_EQUAL_UINT(USER_BLOCKS_PER_DIE - 1 - RESERVED_FREE_BLOCK_COUNT, taken);
	TEST_ASSERT_EQUAL_UINT(RESERVED_FREE_BLOCK_COUNT, virtualDieMapPtr->die[0].freeBlockCnt);
	TEST_ASSERT_NOT_EQUAL(BLOCK_FAIL, GetFromFbList(0, GET_FREE_BLOCK_GC));
	TEST_ASSERT_EQUAL_UINT(0, virtualDieMapPtr->die[0].freeBlockCnt);
	TEST_ASSERT_EQUAL_UINT(BLOCK_NONE, virtualDieMapPtr->die[0].headFreeBlock);
	TEST_ASSERT_EQUAL_UINT(BLOCK_FAIL, GetFromFbList(0, GET_FREE_BLOCK_GC));
}

void test_put_to_fb_list_appends_at_tail(void)
{
	unsigned int block = GetFromFbList(1, GET_FREE_BLOCK_NORMAL);
	unsigned int cntBefore = virtualDieMapPtr->die[1].freeBlockCnt;

	PutToFbList(1, block);

	TEST_ASSERT_EQUAL_UINT(cntBefore + 1, virtualDieMapPtr->die[1].freeBlockCnt);
	TEST_ASSERT_EQUAL_UINT(block, virtualDieMapPtr->die[1].tailFreeBlock);
	TEST_ASSERT_EQUAL_UINT(BLOCK_NONE, virtualBlockMapPtr->block[1][block].nextBlock);
}

/* ---------------------------------------------------------------- InvalidateOldVsa */

void test_invalidate_unmapped_lsa_is_a_noop(void)
{
	unsigned int die, block;
	InvalidateOldVsa(99);
	for (die = 0; die < USER_DIES; die++)
		for (block = 0; block < USER_BLOCKS_PER_DIE; block++)
			TEST_ASSERT_EQUAL_UINT(0, virtualBlockMapPtr->block[die][block].invalidSliceCnt);
}

void test_invalidate_moves_block_onto_gc_victim_list(void)
{
	unsigned int lsa = 10;
	unsigned int vsa = AddrTransWrite(lsa);
	unsigned int die = TestDieOf(vsa), block = TestBlockOf(vsa);

	InvalidateOldVsa(lsa);

	TEST_ASSERT_EQUAL_HEX32(VSA_NONE, logicalSliceMapPtr->logicalSlice[lsa].virtualSliceAddr);
	TEST_ASSERT_EQUAL_UINT(1, virtualBlockMapPtr->block[die][block].invalidSliceCnt);
	TEST_ASSERT_EQUAL_UINT(block, gcVictimMapPtr->gcVictimList[die][1].headBlock);
	TEST_ASSERT_EQUAL_UINT(block, gcVictimMapPtr->gcVictimList[die][1].tailBlock);
	/* The stale reverse mapping is intentionally left in place; GC uses it to spot dead slices. */
	TEST_ASSERT_EQUAL_UINT(lsa, virtualSliceMapPtr->virtualSlice[vsa].logicalSliceAddr);
}

void test_overwrite_invalidates_previous_vsa_and_allocates_new_one(void)
{
	unsigned int lsa = 11;
	unsigned int first = AddrTransWrite(lsa);
	unsigned int second = AddrTransWrite(lsa);
	unsigned int die = TestDieOf(first), block = TestBlockOf(first);

	TEST_ASSERT_NOT_EQUAL(first, second);
	TEST_ASSERT_EQUAL_UINT(second, AddrTransRead(lsa));
	TEST_ASSERT_EQUAL_UINT(1, virtualBlockMapPtr->block[die][block].invalidSliceCnt);
}

void test_repeated_invalidations_rebucket_block_by_invalid_count(void)
{
	unsigned int lsa;
	unsigned int vsa0 = AddrTransWrite(0);
	unsigned int die = TestDieOf(vsa0), block = TestBlockOf(vsa0);

	/* Land three more slices in the same die/block by writing a full round-robin cycle each time. */
	for (lsa = 1; lsa < 3 * USER_DIES; lsa++)
		AddrTransWrite(lsa);

	InvalidateOldVsa(0);
	TEST_ASSERT_EQUAL_UINT(block, gcVictimMapPtr->gcVictimList[die][1].headBlock);

	InvalidateOldVsa(USER_DIES);
	TEST_ASSERT_EQUAL_UINT(BLOCK_NONE, gcVictimMapPtr->gcVictimList[die][1].headBlock);
	TEST_ASSERT_EQUAL_UINT(block, gcVictimMapPtr->gcVictimList[die][2].headBlock);

	InvalidateOldVsa(2 * USER_DIES);
	TEST_ASSERT_EQUAL_UINT(BLOCK_NONE, gcVictimMapPtr->gcVictimList[die][2].headBlock);
	TEST_ASSERT_EQUAL_UINT(block, gcVictimMapPtr->gcVictimList[die][3].headBlock);
	TEST_ASSERT_EQUAL_UINT(3, virtualBlockMapPtr->block[die][block].invalidSliceCnt);
}

void test_invalidate_ignores_stale_forward_mapping(void)
{
	unsigned int lsa = 12;
	unsigned int vsa = AddrTransWrite(lsa);
	unsigned int die = TestDieOf(vsa), block = TestBlockOf(vsa);

	/* Simulate the reverse map having been claimed by another LSA (e.g. after GC). */
	virtualSliceMapPtr->virtualSlice[vsa].logicalSliceAddr = lsa + 1;
	InvalidateOldVsa(lsa);

	TEST_ASSERT_EQUAL_UINT(0, virtualBlockMapPtr->block[die][block].invalidSliceCnt);
	TEST_ASSERT_EQUAL_UINT(vsa, logicalSliceMapPtr->logicalSlice[lsa].virtualSliceAddr);
}

/* ---------------------------------------------------------------- erase */

void test_erase_block_returns_block_to_free_list_and_clears_reverse_map(void)
{
	unsigned int lsa = 20;
	unsigned int vsa = AddrTransWrite(lsa);
	unsigned int die = TestDieOf(vsa), block = TestBlockOf(vsa);
	unsigned int freeBefore = virtualDieMapPtr->die[die].freeBlockCnt;

	InvalidateOldVsa(lsa);
	EraseBlock(die, block);

	TEST_ASSERT_EQUAL_UINT(1, virtualBlockMapPtr->block[die][block].free);
	TEST_ASSERT_EQUAL_UINT(1, virtualBlockMapPtr->block[die][block].eraseCnt);
	TEST_ASSERT_EQUAL_UINT(0, virtualBlockMapPtr->block[die][block].invalidSliceCnt);
	TEST_ASSERT_EQUAL_UINT(0, virtualBlockMapPtr->block[die][block].currentPage);
	TEST_ASSERT_EQUAL_UINT(freeBefore + 1, virtualDieMapPtr->die[die].freeBlockCnt);
	TEST_ASSERT_EQUAL_HEX32(LSA_NONE, virtualSliceMapPtr->virtualSlice[vsa].logicalSliceAddr);
	/* The slice was mapped but never programmed on NAND, so the erase must wait behind the
	 * outstanding program in the row-address dependency queue rather than reach the die. */
	TEST_ASSERT_EQUAL_UINT(0, TestCountNandReqs(TestChannelOfDie(die), TestWayOfDie(die), REQ_CODE_ERASE));
	TEST_ASSERT_EQUAL_UINT(1, blockedByRowAddrDepReqQ[TestChannelOfDie(die)][TestWayOfDie(die)].reqCnt);
	TEST_ASSERT_EQUAL_UINT(1, rowAddrDependencyTablePtr->block[TestChannelOfDie(die)][TestWayOfDie(die)][block].blockedEraseReqFlag);
}

void test_erase_of_unused_block_is_issued_immediately(void)
{
	unsigned int block = GetFromFbList(2, GET_FREE_BLOCK_NORMAL);
	EraseBlock(2, block);
	TEST_ASSERT_EQUAL_UINT(1, TestCountNandReqs(TestChannelOfDie(2), TestWayOfDie(2), REQ_CODE_ERASE));
	TEST_ASSERT_EQUAL_UINT(0, blockedByRowAddrDepReqQ[TestChannelOfDie(2)][TestWayOfDie(2)].reqCnt);
}

/* ---------------------------------------------------------------- bad-block remap */

void test_grown_bad_block_is_booked_in_bad_block_table_info(void)
{
	UpdatePhyBlockMapForGrownBadBlock(2, 17);
	TEST_ASSERT_EQUAL_UINT(BLOCK_STATE_BAD, phyBlockMapPtr->phyBlock[2][17].bad);
	TEST_ASSERT_EQUAL_UINT(BBT_INFO_GROWN_BAD_UPDATE_BOOKED, bbtInfoMapPtr->bbtInfo[2].grownBadUpdate);
	TEST_ASSERT_EQUAL_UINT(BBT_INFO_GROWN_BAD_UPDATE_NONE, bbtInfoMapPtr->bbtInfo[3].grownBadUpdate);
}

void test_remap_bad_block_redirects_to_reserved_block_in_same_lun(void)
{
	unsigned int die = 1;
	unsigned int badBlock = 5;
	unsigned int remapped;

	phyBlockMapPtr->phyBlock[die][badBlock].bad = BLOCK_STATE_BAD;
	RemapBadBlock();

	remapped = phyBlockMapPtr->phyBlock[die][badBlock].remappedPhyBlock;
	TEST_ASSERT_NOT_EQUAL(badBlock, remapped);
	TEST_ASSERT_TRUE(remapped >= USER_BLOCKS_PER_LUN);
	TEST_ASSERT_TRUE(remapped < TOTAL_BLOCKS_PER_LUN);
	TEST_ASSERT_EQUAL_UINT(BLOCK_STATE_NORMAL, phyBlockMapPtr->phyBlock[die][remapped].bad);
	/* Untouched dies / blocks keep the identity mapping. */
	TEST_ASSERT_EQUAL_UINT(badBlock, phyBlockMapPtr->phyBlock[0][badBlock].remappedPhyBlock);
	TEST_ASSERT_EQUAL_UINT(badBlock + 1, phyBlockMapPtr->phyBlock[die][badBlock + 1].remappedPhyBlock);
}

void test_remapped_bad_block_routes_row_address_to_replacement(void)
{
	unsigned int die = 0, badBlock = 9, remapped, vsa, tag;

	phyBlockMapPtr->phyBlock[die][badBlock].bad = BLOCK_STATE_BAD;
	RemapBadBlock();
	remapped = phyBlockMapPtr->phyBlock[die][badBlock].remappedPhyBlock;

	vsa = Vorg2VsaTranslation(die, badBlock, 4);
	tag = MakeVsaReq(vsa);
	TEST_ASSERT_EQUAL_HEX32(LUN_0_BASE_ADDR + remapped * PAGES_PER_MLC_BLOCK + Vpage2PlsbPageTranslation(4),
							GenerateNandRowAddr(tag));
}

void test_full_init_with_factory_bad_block_scans_remaps_and_persists_table(void)
{
	unsigned int die = 0, ch = TestChannelOfDie(die), way = TestWayOfDie(die);
	unsigned int badBlock = 40, remapped, bbtBlock;

	FakeNandReset();
	FakeNandMarkBadBlock(ch, way, badBlock);
	TestFtlResetWithFullInitKeepingNand();

	TEST_ASSERT_EQUAL_UINT(BLOCK_STATE_BAD, phyBlockMapPtr->phyBlock[die][badBlock].bad);
	remapped = phyBlockMapPtr->phyBlock[die][badBlock].remappedPhyBlock;
	TEST_ASSERT_NOT_EQUAL(badBlock, remapped);
	TEST_ASSERT_TRUE(remapped >= USER_BLOCKS_PER_LUN);
	TEST_ASSERT_EQUAL_UINT(BLOCK_STATE_NORMAL, virtualBlockMapPtr->block[die][badBlock].bad);
	TEST_ASSERT_EQUAL_UINT(BLOCK_STATE_NORMAL, phyBlockMapPtr->phyBlock[1][badBlock].bad);

	/* The scan result was written back to NAND as the bad block table. */
	bbtBlock = bbtInfoMapPtr->bbtInfo[die].phyBlock;
	TEST_ASSERT_TRUE(FakeNandIsProgrammed(ch, way, FakeNandPhyBlockToRow(bbtBlock, START_PAGE_NO_OF_BAD_BLOCK_TABLE_BLOCK)));

	/* Wiping the fake-NAND bad list must not change the outcome: on the second boot the
	 * persisted table is loaded instead of rescanning. */
	TestFtlResetWithFullInitKeepingNand();
	TEST_ASSERT_EQUAL_UINT(BLOCK_STATE_BAD, phyBlockMapPtr->phyBlock[die][badBlock].bad);
	TEST_ASSERT_EQUAL_UINT(remapped, phyBlockMapPtr->phyBlock[die][badBlock].remappedPhyBlock);
}

int main(void)
{
	UNITY_BEGIN();
	RUN_TEST(test_unwritten_lsa_reads_as_vsa_fail);
	RUN_TEST(test_write_then_read_round_trips_through_both_maps);
	RUN_TEST(test_vsa_decomposes_into_die_block_page_and_back);
	RUN_TEST(test_vsa_maps_to_physical_row_address);
	RUN_TEST(test_second_lun_vsa_uses_lun1_row_base);
	RUN_TEST(test_find_free_virtual_slice_round_robins_across_dies);
	RUN_TEST(test_find_free_virtual_slice_advances_current_page);
	RUN_TEST(test_find_free_virtual_slice_moves_to_next_free_block_when_full);
	RUN_TEST(test_free_block_list_holds_all_but_current_block_after_init);
	RUN_TEST(test_get_from_fb_list_normal_keeps_reserved_blocks_for_gc);
	RUN_TEST(test_put_to_fb_list_appends_at_tail);
	RUN_TEST(test_invalidate_unmapped_lsa_is_a_noop);
	RUN_TEST(test_invalidate_moves_block_onto_gc_victim_list);
	RUN_TEST(test_overwrite_invalidates_previous_vsa_and_allocates_new_one);
	RUN_TEST(test_repeated_invalidations_rebucket_block_by_invalid_count);
	RUN_TEST(test_invalidate_ignores_stale_forward_mapping);
	RUN_TEST(test_erase_block_returns_block_to_free_list_and_clears_reverse_map);
	RUN_TEST(test_erase_of_unused_block_is_issued_immediately);
	RUN_TEST(test_grown_bad_block_is_booked_in_bad_block_table_info);
	RUN_TEST(test_remap_bad_block_redirects_to_reserved_block_in_same_lun);
	RUN_TEST(test_remapped_bad_block_routes_row_address_to_replacement);
	RUN_TEST(test_full_init_with_factory_bad_block_scans_remaps_and_persists_table);
	return UNITY_END();
}
