#include "unity.h"

#include "ftl_test_env.h"

#define TEST_LSA 1234u

void setUp(void)
{
	ftl_test_env_init();
}

void tearDown(void)
{
}

/* Physical row address the scheduler would program for a VSA, via the real translation path. */
static unsigned int RowAddrForVsa(unsigned int vsa, unsigned int *ch, unsigned int *way)
{
	unsigned int tag = GetFromFreeReqQ();
	unsigned int row;
	reqPoolPtr->reqPool[tag].reqOpt.nandAddr = REQ_OPT_NAND_ADDR_VSA;
	reqPoolPtr->reqPool[tag].nandInfo.virtualSliceAddr = vsa;
	row = GenerateNandRowAddr(tag);
	*ch = Vdie2PchTranslation(Vsa2VdieTranslation(vsa));
	*way = Vdie2PwayTranslation(Vsa2VdieTranslation(vsa));
	PutToFreeReqQ(tag);
	return row;
}

/* ---- LBA -> LSA -> VSA -> PSA ------------------------------------------- */

static void test_read_of_unwritten_lsa_fails(void)
{
	TEST_ASSERT_EQUAL_HEX32(VSA_FAIL, AddrTransRead(TEST_LSA));
	TEST_ASSERT_EQUAL_HEX32(VSA_FAIL, AddrTransRead(0));
	TEST_ASSERT_EQUAL_HEX32(VSA_FAIL, AddrTransRead(SLICES_PER_SSD - 1));
}

static void test_write_then_read_round_trips_through_both_maps(void)
{
	unsigned int vsa = AddrTransWrite(TEST_LSA);

	TEST_ASSERT_NOT_EQUAL(VSA_FAIL, vsa);
	TEST_ASSERT_EQUAL_HEX32(vsa, AddrTransRead(TEST_LSA));
	TEST_ASSERT_EQUAL_UINT32(vsa, logicalSliceMapPtr->logicalSlice[TEST_LSA].virtualSliceAddr);
	TEST_ASSERT_EQUAL_UINT32(TEST_LSA, virtualSliceMapPtr->virtualSlice[vsa].logicalSliceAddr);
}

static void test_first_vsa_lands_on_target_die_current_block_page0(void)
{
	unsigned int targetDie = sliceAllocationTargetDie;
	unsigned int vsa = AddrTransWrite(TEST_LSA);
	unsigned int die = Vsa2VdieTranslation(vsa);
	unsigned int block = Vsa2VblockTranslation(vsa);
	unsigned int page = Vsa2VpageTranslation(vsa);

	TEST_ASSERT_EQUAL_UINT32(targetDie, die);
	TEST_ASSERT_EQUAL_UINT32(virtualDieMapPtr->die[die].currentBlock, block);
	TEST_ASSERT_EQUAL_UINT32(0, page);
	TEST_ASSERT_EQUAL_UINT32(1, virtualBlockMapPtr->block[die][block].currentPage);
	TEST_ASSERT_EQUAL_HEX32(vsa, Vorg2VsaTranslation(die, block, page));
}

static void test_vsa_to_psa_follows_remap_table_and_lsb_pages(void)
{
	unsigned int vsa = AddrTransWrite(TEST_LSA);
	unsigned int die = Vsa2VdieTranslation(vsa);
	unsigned int vblock = Vsa2VblockTranslation(vsa);
	unsigned int vpage = Vsa2VpageTranslation(vsa);
	unsigned int pblock = Vblock2PblockOfTbsTranslation(vblock);
	unsigned int ch, way, row;

	/* Physical block 0 hosts the bad-block table and is hidden via the remap table;
	 * every other block on a clean device maps to itself. */
	if (pblock == bbtInfoMapPtr->bbtInfo[die].phyBlock)
		TEST_ASSERT_EQUAL_UINT32(USER_BLOCKS_PER_LUN, phyBlockMapPtr->phyBlock[die][pblock].remappedPhyBlock);
	else
		TEST_ASSERT_EQUAL_UINT32(pblock, phyBlockMapPtr->phyBlock[die][pblock].remappedPhyBlock);
	pblock = phyBlockMapPtr->phyBlock[die][pblock].remappedPhyBlock;

	row = RowAddrForVsa(vsa, &ch, &way);
	TEST_ASSERT_EQUAL_UINT32(die % USER_CHANNELS, ch);
	TEST_ASSERT_EQUAL_UINT32(die / USER_CHANNELS, way);
	/* SLC mode: virtual page N is stored in LSB page 2N-1 (page 0 stays 0). */
	TEST_ASSERT_EQUAL_HEX32(fake_nand_row_addr(pblock, Vpage2PlsbPageTranslation(vpage)), row);
}

static void test_psa_selects_lun1_for_upper_virtual_blocks(void)
{
	unsigned int vblock = USER_BLOCKS_PER_LUN + 3;
	unsigned int vsa = Vorg2VsaTranslation(0, vblock, 5);
	unsigned int ch, way, row = RowAddrForVsa(vsa, &ch, &way);

	TEST_ASSERT_TRUE(row >= LUN_1_BASE_ADDR);
	TEST_ASSERT_EQUAL_HEX32(LUN_1_BASE_ADDR + 3 * PAGES_PER_MLC_BLOCK + Vpage2PlsbPageTranslation(5), row);
}

static void test_lba_to_lsa_slicing_matches_nvme_block_geometry(void)
{
	/* NVME_BLOCKS_PER_SLICE LBAs share one slice; ReqTransNvmeToSlice splits at slice boundaries. */
	unsigned int lba = 7 * NVME_BLOCKS_PER_SLICE + 1;
	unsigned int tag;

	ReqTransNvmeToSlice(0, lba, NVME_BLOCKS_PER_SLICE, IO_NVM_READ);
	TEST_ASSERT_EQUAL_UINT32(2, sliceReqQ.reqCnt);

	tag = sliceReqQ.headReq;
	TEST_ASSERT_EQUAL_UINT32(7, reqPoolPtr->reqPool[tag].logicalSliceAddr);
	TEST_ASSERT_EQUAL_UINT32(1, reqPoolPtr->reqPool[tag].nvmeDmaInfo.nvmeBlockOffset);
	TEST_ASSERT_EQUAL_UINT32(NVME_BLOCKS_PER_SLICE - 1, reqPoolPtr->reqPool[tag].nvmeDmaInfo.numOfNvmeBlock);

	tag = reqPoolPtr->reqPool[tag].nextReq;
	TEST_ASSERT_EQUAL_UINT32(8, reqPoolPtr->reqPool[tag].logicalSliceAddr);
	TEST_ASSERT_EQUAL_UINT32(2, reqPoolPtr->reqPool[tag].nvmeDmaInfo.numOfNvmeBlock);
}

/* ---- FindFreeVirtualSlice ----------------------------------------------- */

static void test_find_free_virtual_slice_round_robins_dies_channel_first(void)
{
	/* The die cursor lives in static state inside FindDieForFreeSliceAllocation(),
	 * so the walk is checked relative to wherever it currently points. */
	unsigned int start = sliceAllocationTargetDie;
	unsigned int startCh = Vdie2PchTranslation(start), startWay = Vdie2PwayTranslation(start);
	unsigned int i;
	for (i = 0; i < USER_DIES * 2; i++)
	{
		unsigned int vsa = FindFreeVirtualSlice();
		unsigned int die = Vsa2VdieTranslation(vsa);
		unsigned int ch = (startCh + i) % USER_CHANNELS;
		unsigned int way = (startWay + (startCh + i) / USER_CHANNELS) % USER_WAYS;
		TEST_ASSERT_EQUAL_UINT32(Pcw2VdieTranslation(ch, way), die);
		TEST_ASSERT_EQUAL_UINT32(i / USER_DIES, Vsa2VpageTranslation(vsa));
	}
	TEST_ASSERT_EQUAL_UINT32(start, sliceAllocationTargetDie);
}

static void test_find_free_virtual_slice_moves_to_next_block_when_full(void)
{
	unsigned int die = sliceAllocationTargetDie;
	unsigned int firstBlock = virtualDieMapPtr->die[die].currentBlock;
	unsigned int freeBefore = virtualDieMapPtr->die[die].freeBlockCnt;
	unsigned int i, vsa = VSA_NONE;

	for (i = 0; i < USER_PAGES_PER_BLOCK * USER_DIES; i++)
		vsa = FindFreeVirtualSlice();

	TEST_ASSERT_EQUAL_UINT32(USER_PAGES_PER_BLOCK, virtualBlockMapPtr->block[die][firstBlock].currentPage);
	TEST_ASSERT_EQUAL_UINT32(USER_PAGES_PER_BLOCK - 1, Vsa2VpageTranslation(vsa));

	vsa = FindFreeVirtualSlice();
	TEST_ASSERT_EQUAL_UINT32(die, Vsa2VdieTranslation(vsa));
	TEST_ASSERT_NOT_EQUAL(firstBlock, Vsa2VblockTranslation(vsa));
	TEST_ASSERT_EQUAL_UINT32(virtualDieMapPtr->die[die].currentBlock, Vsa2VblockTranslation(vsa));
	TEST_ASSERT_EQUAL_UINT32(0, Vsa2VpageTranslation(vsa));
	TEST_ASSERT_EQUAL_UINT32(freeBefore - 1, virtualDieMapPtr->die[die].freeBlockCnt);
	TEST_ASSERT_EQUAL_UINT32(0, virtualBlockMapPtr->block[die][virtualDieMapPtr->die[die].currentBlock].free);
}

static void test_free_block_list_reserves_last_block_for_gc(void)
{
	unsigned int die = 0, block;

	while (virtualDieMapPtr->die[die].freeBlockCnt > RESERVED_FREE_BLOCK_COUNT)
	{
		block = GetFromFbList(die, GET_FREE_BLOCK_NORMAL);
		TEST_ASSERT_NOT_EQUAL(BLOCK_FAIL, block);
	}
	TEST_ASSERT_EQUAL_UINT32(BLOCK_FAIL, GetFromFbList(die, GET_FREE_BLOCK_NORMAL));
	TEST_ASSERT_NOT_EQUAL(BLOCK_FAIL, GetFromFbList(die, GET_FREE_BLOCK_GC));
	TEST_ASSERT_EQUAL_UINT32(BLOCK_FAIL, GetFromFbList(die, GET_FREE_BLOCK_GC));
	TEST_ASSERT_EQUAL_UINT32(BLOCK_NONE, virtualDieMapPtr->die[die].headFreeBlock);
}

/* ---- InvalidateOldVsa --------------------------------------------------- */

static void test_invalidate_unmapped_lsa_is_noop(void)
{
	unsigned int die = sliceAllocationTargetDie;
	InvalidateOldVsa(TEST_LSA);
	TEST_ASSERT_EQUAL_HEX32(VSA_NONE, logicalSliceMapPtr->logicalSlice[TEST_LSA].virtualSliceAddr);
	TEST_ASSERT_EQUAL_UINT32(0, virtualBlockMapPtr->block[die][virtualDieMapPtr->die[die].currentBlock].invalidSliceCnt);
}

static void test_invalidate_bumps_invalid_count_and_moves_block_to_victim_list(void)
{
	unsigned int vsa = AddrTransWrite(TEST_LSA);
	unsigned int die = Vsa2VdieTranslation(vsa);
	unsigned int block = Vsa2VblockTranslation(vsa);

	InvalidateOldVsa(TEST_LSA);

	TEST_ASSERT_EQUAL_HEX32(VSA_NONE, logicalSliceMapPtr->logicalSlice[TEST_LSA].virtualSliceAddr);
	TEST_ASSERT_EQUAL_HEX32(VSA_FAIL, AddrTransRead(TEST_LSA));
	TEST_ASSERT_EQUAL_UINT32(1, virtualBlockMapPtr->block[die][block].invalidSliceCnt);
	TEST_ASSERT_EQUAL_UINT32(block, gcVictimMapPtr->gcVictimList[die][1].headBlock);
	TEST_ASSERT_EQUAL_UINT32(block, gcVictimMapPtr->gcVictimList[die][1].tailBlock);
	/* The stale reverse mapping is left in place until the block is erased. */
	TEST_ASSERT_EQUAL_UINT32(TEST_LSA, virtualSliceMapPtr->virtualSlice[vsa].logicalSliceAddr);
}

static void test_overwrite_invalidates_previous_vsa(void)
{
	unsigned int vsa1 = AddrTransWrite(TEST_LSA);
	unsigned int vsa2 = AddrTransWrite(TEST_LSA);
	unsigned int die = Vsa2VdieTranslation(vsa1);
	unsigned int block = Vsa2VblockTranslation(vsa1);

	TEST_ASSERT_NOT_EQUAL(vsa1, vsa2);
	TEST_ASSERT_EQUAL_HEX32(vsa2, AddrTransRead(TEST_LSA));
	TEST_ASSERT_EQUAL_UINT32(1, virtualBlockMapPtr->block[die][block].invalidSliceCnt);
	TEST_ASSERT_EQUAL_UINT32(block, gcVictimMapPtr->gcVictimList[die][1].headBlock);
}

static void test_invalidate_relinks_block_between_victim_lists(void)
{
	unsigned int vsa = AddrTransWrite(TEST_LSA);
	unsigned int die = Vsa2VdieTranslation(vsa);
	unsigned int block = Vsa2VblockTranslation(vsa);
	unsigned int lsa2 = TEST_LSA + USER_DIES;
	unsigned int i;

	/* Keep writing until the die cursor wraps back to the first slice's die. */
	for (i = 0; i < USER_DIES; i++)
	{
		lsa2 = TEST_LSA + USER_DIES + i;
		if (Vsa2VdieTranslation(AddrTransWrite(lsa2)) == die)
			break;
	}
	TEST_ASSERT_EQUAL_UINT32(die, Vsa2VdieTranslation(AddrTransRead(lsa2)));
	TEST_ASSERT_EQUAL_UINT32(block, Vsa2VblockTranslation(AddrTransRead(lsa2)));

	InvalidateOldVsa(TEST_LSA);
	TEST_ASSERT_EQUAL_UINT32(block, gcVictimMapPtr->gcVictimList[die][1].headBlock);
	InvalidateOldVsa(lsa2);
	TEST_ASSERT_EQUAL_UINT32(BLOCK_NONE, gcVictimMapPtr->gcVictimList[die][1].headBlock);
	TEST_ASSERT_EQUAL_UINT32(block, gcVictimMapPtr->gcVictimList[die][2].headBlock);
	TEST_ASSERT_EQUAL_UINT32(2, virtualBlockMapPtr->block[die][block].invalidSliceCnt);
}

static void test_invalidate_skips_slice_owned_by_another_lsa(void)
{
	unsigned int vsa = AddrTransWrite(TEST_LSA);
	unsigned int die = Vsa2VdieTranslation(vsa);
	unsigned int block = Vsa2VblockTranslation(vsa);

	/* Simulate a stale forward mapping (e.g. after GC re-homed the slice). */
	virtualSliceMapPtr->virtualSlice[vsa].logicalSliceAddr = TEST_LSA + 1;
	InvalidateOldVsa(TEST_LSA);

	TEST_ASSERT_EQUAL_UINT32(0, virtualBlockMapPtr->block[die][block].invalidSliceCnt);
	TEST_ASSERT_EQUAL_HEX32(vsa, logicalSliceMapPtr->logicalSlice[TEST_LSA].virtualSliceAddr);
}

/* ---- Bad-block remapping ------------------------------------------------ */

static void test_factory_bad_block_is_remapped_to_first_reserved_block(void)
{
	unsigned int badBlock = 17, die;

	ftl_test_env_init_with_bad_block(badBlock);

	for (die = 0; die < USER_DIES; die++)
	{
		TEST_ASSERT_EQUAL_UINT32(BLOCK_STATE_BAD, phyBlockMapPtr->phyBlock[die][badBlock].bad);
		/* Reserved block USER_BLOCKS_PER_LUN is already taken by the BBT block remap. */
		TEST_ASSERT_EQUAL_UINT32(USER_BLOCKS_PER_LUN + 1, phyBlockMapPtr->phyBlock[die][badBlock].remappedPhyBlock);
		/* The virtual block stays usable because it now points at a good reserved block. */
		TEST_ASSERT_EQUAL_UINT32(BLOCK_STATE_NORMAL, virtualBlockMapPtr->block[die][badBlock].bad);
	}
	TEST_ASSERT_EQUAL_UINT32(0, mbPerbadBlockSpace);
}

static void test_remapped_block_programs_reserved_physical_block(void)
{
	unsigned int badBlock = 17, ch, way, row;
	unsigned int vsa = Vorg2VsaTranslation(0, badBlock, 0);

	ftl_test_env_init_with_bad_block(badBlock);

	row = RowAddrForVsa(vsa, &ch, &way);
	TEST_ASSERT_EQUAL_HEX32(fake_nand_row_addr(USER_BLOCKS_PER_LUN + 1, 0), row);
}

static void test_bad_block_in_lun1_uses_lun1_reserved_area(void)
{
	unsigned int badBlock = TOTAL_BLOCKS_PER_LUN + 9;

	ftl_test_env_init_with_bad_block(badBlock);

	TEST_ASSERT_EQUAL_UINT32(TOTAL_BLOCKS_PER_LUN + USER_BLOCKS_PER_LUN,
							 phyBlockMapPtr->phyBlock[0][badBlock].remappedPhyBlock);
}

static void test_bbt_block_is_hidden_from_host(void)
{
	unsigned int die;
	for (die = 0; die < USER_DIES; die++)
	{
		unsigned int bbtBlock = bbtInfoMapPtr->bbtInfo[die].phyBlock;
		TEST_ASSERT_EQUAL_UINT32(BLOCK_STATE_BAD, phyBlockMapPtr->phyBlock[die][bbtBlock].bad);
		TEST_ASSERT_EQUAL_UINT32(USER_BLOCKS_PER_LUN, phyBlockMapPtr->phyBlock[die][bbtBlock].remappedPhyBlock);
	}
}

static void test_grown_bad_block_is_booked_for_bbt_update(void)
{
	UpdatePhyBlockMapForGrownBadBlock(3, 100);

	TEST_ASSERT_EQUAL_UINT32(BLOCK_STATE_BAD, phyBlockMapPtr->phyBlock[3][100].bad);
	TEST_ASSERT_EQUAL_UINT32(BBT_INFO_GROWN_BAD_UPDATE_BOOKED, bbtInfoMapPtr->bbtInfo[3].grownBadUpdate);
	TEST_ASSERT_EQUAL_UINT32(BBT_INFO_GROWN_BAD_UPDATE_NONE, bbtInfoMapPtr->bbtInfo[2].grownBadUpdate);
}

static void test_program_failure_marks_physical_block_bad(void)
{
	unsigned int vsa = AddrTransWrite(TEST_LSA);
	unsigned int die = Vsa2VdieTranslation(vsa);
	unsigned int pblock = Vblock2PblockOfTbsTranslation(Vsa2VblockTranslation(vsa));
	unsigned int tag = GetFromFreeReqQ();

	fake_nand_fail_next_program(Vdie2PchTranslation(die), Vdie2PwayTranslation(die));

	reqPoolPtr->reqPool[tag].reqType = REQ_TYPE_NAND;
	reqPoolPtr->reqPool[tag].reqCode = REQ_CODE_WRITE;
	reqPoolPtr->reqPool[tag].reqOpt.nandAddr = REQ_OPT_NAND_ADDR_VSA;
	reqPoolPtr->reqPool[tag].reqOpt.dataBufFormat = REQ_OPT_DATA_BUF_TEMP_ENTRY;
	reqPoolPtr->reqPool[tag].reqOpt.nandEcc = REQ_OPT_NAND_ECC_ON;
	reqPoolPtr->reqPool[tag].reqOpt.nandEccWarning = REQ_OPT_NAND_ECC_WARNING_OFF;
	reqPoolPtr->reqPool[tag].reqOpt.rowAddrDependencyCheck = REQ_OPT_ROW_ADDR_DEPENDENCY_NONE;
	reqPoolPtr->reqPool[tag].reqOpt.blockSpace = REQ_OPT_BLOCK_SPACE_MAIN;
	reqPoolPtr->reqPool[tag].dataBufInfo.entry = AllocateTempDataBuf(die);
	reqPoolPtr->reqPool[tag].nandInfo.virtualSliceAddr = vsa;
	SelectLowLevelReqQ(tag);
	ftl_test_drain();

	TEST_ASSERT_EQUAL_UINT32(BLOCK_STATE_BAD, phyBlockMapPtr->phyBlock[die][pblock].bad);
	TEST_ASSERT_EQUAL_UINT32(BBT_INFO_GROWN_BAD_UPDATE_BOOKED, bbtInfoMapPtr->bbtInfo[die].grownBadUpdate);
}

int main(void)
{
	UNITY_BEGIN();
	RUN_TEST(test_read_of_unwritten_lsa_fails);
	RUN_TEST(test_write_then_read_round_trips_through_both_maps);
	RUN_TEST(test_first_vsa_lands_on_target_die_current_block_page0);
	RUN_TEST(test_vsa_to_psa_follows_remap_table_and_lsb_pages);
	RUN_TEST(test_psa_selects_lun1_for_upper_virtual_blocks);
	RUN_TEST(test_lba_to_lsa_slicing_matches_nvme_block_geometry);
	RUN_TEST(test_find_free_virtual_slice_round_robins_dies_channel_first);
	RUN_TEST(test_find_free_virtual_slice_moves_to_next_block_when_full);
	RUN_TEST(test_free_block_list_reserves_last_block_for_gc);
	RUN_TEST(test_invalidate_unmapped_lsa_is_noop);
	RUN_TEST(test_invalidate_bumps_invalid_count_and_moves_block_to_victim_list);
	RUN_TEST(test_overwrite_invalidates_previous_vsa);
	RUN_TEST(test_invalidate_relinks_block_between_victim_lists);
	RUN_TEST(test_invalidate_skips_slice_owned_by_another_lsa);
	RUN_TEST(test_factory_bad_block_is_remapped_to_first_reserved_block);
	RUN_TEST(test_remapped_block_programs_reserved_physical_block);
	RUN_TEST(test_bad_block_in_lun1_uses_lun1_reserved_area);
	RUN_TEST(test_bbt_block_is_hidden_from_host);
	RUN_TEST(test_grown_bad_block_is_booked_for_bbt_update);
	RUN_TEST(test_program_failure_marks_physical_block_bad);
	return UNITY_END();
}
