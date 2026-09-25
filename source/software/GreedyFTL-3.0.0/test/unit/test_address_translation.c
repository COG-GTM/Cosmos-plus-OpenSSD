/* Unit tests for address_translation.c: LBA->VSA->PSA mapping, slice
 * allocation, free block list, bad block remapping and bad block table
 * recovery / persistence. Every test boots the full FTL against the mocked
 * NAND array in setUp() so the real tables are populated. */
#include <string.h>
#include "unity.h"
#include "ftl_test_env.h"
#include "ftl_config.h"
#include "memory_map.h"
#include "address_translation.h"
#include "garbage_collection.h"
#include "request_schedule.h"

/* Boot-time helpers that address_translation.h does not export. */
void InitDieMap(void);
void InitBlockMap(void);
void InitCurrentBlockOfDieMap(void);
void RemapBadBlock(void);
void RecoverBadBlockTable(unsigned int tempBufAddr);
void EraseTotalBlockSpace(void);

#define BBT_BUF_ENTRY_SIZE   (BYTES_PER_DATA_REGION_OF_PAGE + BYTES_PER_SPARE_REGION_OF_PAGE)
#define BBT_MARK_CLEAN       CLEAN_DATA_IN_BYTE
#define BBT_MARK_BAD         0x00u
#define FIRST_RESERVED_LUN0  (USER_BLOCKS_PER_LUN)
#define FIRST_RESERVED_LUN1  (TOTAL_BLOCKS_PER_LUN + USER_BLOCKS_PER_LUN)

/* Page image returned by the NAND mock for bad block table / bad mark reads. */
static unsigned char nandPage[BBT_BUF_ENTRY_SIZE];

void setUp(void) { ftl_test_env_reset(); ftl_test_env_init_ftl(); }
void tearDown(void) {}

/* ---------------------------------------------------------------- helpers */

static unsigned int fbListLength(unsigned int dieNo)
{
	unsigned int n = 0, b = virtualDieMapPtr->die[dieNo].headFreeBlock;
	while (b != BLOCK_NONE)
	{
		n++;
		b = virtualBlockMapPtr->block[dieNo][b].nextBlock;
	}
	return n;
}

/* Take blocks from the free list until only RESERVED_FREE_BLOCK_COUNT remain. */
static void drainFreeBlocks(unsigned int dieNo)
{
	while (GetFromFbList(dieNo, GET_FREE_BLOCK_NORMAL) != BLOCK_FAIL)
		;
	TEST_ASSERT_EQUAL_UINT(RESERVED_FREE_BLOCK_COUNT, virtualDieMapPtr->die[dieNo].freeBlockCnt);
}

static void resetPhyBlockMapOfDie(unsigned int dieNo)
{
	unsigned int b;
	for (b = 0; b < TOTAL_BLOCKS_PER_DIE; b++)
	{
		phyBlockMapPtr->phyBlock[dieNo][b].remappedPhyBlock = b;
		phyBlockMapPtr->phyBlock[dieNo][b].bad = 0;
	}
}

static void markBad(unsigned int dieNo, unsigned int phyBlockNo)
{
	phyBlockMapPtr->phyBlock[dieNo][phyBlockNo].bad = 1;
}

static void setDie0ReadPage(unsigned char tableByte, unsigned char markByte0, unsigned char markByte1)
{
	memset(nandPage, CLEAN_DATA_IN_BYTE, sizeof(nandPage));
	nandPage[0] = tableByte;
	nandPage[BAD_BLOCK_MARK_BYTE0] = markByte0;
	nandPage[BAD_BLOCK_MARK_BYTE1] = markByte1;
	mock_nsc_set_read_page_source(chCtlReg[Vdie2PchTranslation(0)], Vdie2PwayTranslation(0),
	                              nandPage, sizeof(nandPage));
}

static unsigned char *bbtBufOfDie(unsigned int dieNo)
{
	return (unsigned char *)(uintptr_t)(RESERVED_DATA_BUFFER_BASE_ADDR
	                                    + dieNo * USED_PAGES_FOR_BAD_BLOCK_TABLE_PER_DIE * BBT_BUF_ENTRY_SIZE);
}

/* ------------------------------------------------------ translation macros */

static void test_vsa_to_vorg_macros_invert_vorg_to_vsa(void)
{
	unsigned int die = USER_DIES - 1, block = 123, page = USER_PAGES_PER_BLOCK - 1;
	unsigned int vsa = Vorg2VsaTranslation(die, block, page);

	TEST_ASSERT_EQUAL_UINT(die, Vsa2VdieTranslation(vsa));
	TEST_ASSERT_EQUAL_UINT(block, Vsa2VblockTranslation(vsa));
	TEST_ASSERT_EQUAL_UINT(page, Vsa2VpageTranslation(vsa));
	TEST_ASSERT_EQUAL_UINT(0, Vorg2VsaTranslation(0, 0, 0));
	TEST_ASSERT_EQUAL_UINT(USER_DIES, Vorg2VsaTranslation(0, 0, 1));
}

static void test_die_to_channel_way_macros_are_channel_interleaved(void)
{
	unsigned int die;
	for (die = 0; die < USER_DIES; die++)
	{
		unsigned int ch = Vdie2PchTranslation(die), way = Vdie2PwayTranslation(die);
		TEST_ASSERT_TRUE(ch < USER_CHANNELS);
		TEST_ASSERT_TRUE(way < USER_WAYS);
		TEST_ASSERT_EQUAL_UINT(die, Pcw2VdieTranslation(ch, way));
	}
	TEST_ASSERT_EQUAL_UINT(1, Vdie2PchTranslation(1));
	TEST_ASSERT_EQUAL_UINT(0, Vdie2PwayTranslation(1));
	TEST_ASSERT_EQUAL_UINT(1, Vdie2PwayTranslation(USER_CHANNELS));
}

static void test_virtual_block_to_physical_block_skips_extended_blocks_of_lun0(void)
{
	TEST_ASSERT_EQUAL_UINT(7, Vblock2PblockOfTbsTranslation(7));
	TEST_ASSERT_EQUAL_UINT(TOTAL_BLOCKS_PER_LUN, Vblock2PblockOfTbsTranslation(USER_BLOCKS_PER_LUN));
	TEST_ASSERT_EQUAL_UINT(TOTAL_BLOCKS_PER_LUN + 5, Vblock2PblockOfTbsTranslation(USER_BLOCKS_PER_LUN + 5));
	TEST_ASSERT_EQUAL_UINT(MAIN_BLOCKS_PER_LUN + 5, Vblock2PblockOfMbsTranslation(USER_BLOCKS_PER_LUN + 5));
}

static void test_lsb_page_translation_round_trips(void)
{
	unsigned int page;
	TEST_ASSERT_EQUAL_UINT(0, Vpage2PlsbPageTranslation(0));
	TEST_ASSERT_EQUAL_UINT(1, Vpage2PlsbPageTranslation(1));
	TEST_ASSERT_EQUAL_UINT(3, Vpage2PlsbPageTranslation(2));
	TEST_ASSERT_EQUAL_UINT(0, PlsbPage2VpageTranslation(0));
	for (page = 0; page < 64; page++)
		TEST_ASSERT_EQUAL_UINT(page, PlsbPage2VpageTranslation(Vpage2PlsbPageTranslation(page)));
}

/* ------------------------------------------------------------ InitAddressMap */

static void test_init_address_map_leaves_every_logical_slice_unmapped(void)
{
	unsigned int lsa, die;

	for (lsa = 0; lsa < SLICES_PER_SSD; lsa += SLICES_PER_SSD / 97)
	{
		TEST_ASSERT_EQUAL_HEX32(VSA_NONE, logicalSliceMapPtr->logicalSlice[lsa].virtualSliceAddr);
		TEST_ASSERT_EQUAL_HEX32(LSA_NONE, virtualSliceMapPtr->virtualSlice[lsa].logicalSliceAddr);
	}
	for (die = 0; die < USER_DIES; die++)
	{
		TEST_ASSERT_EQUAL_UINT(0, bbtInfoMapPtr->bbtInfo[die].phyBlock);
		TEST_ASSERT_EQUAL_UINT(BBT_INFO_GROWN_BAD_UPDATE_NONE, bbtInfoMapPtr->bbtInfo[die].grownBadUpdate);
		TEST_ASSERT_NOT_EQUAL(BLOCK_NONE, virtualDieMapPtr->die[die].currentBlock);
		TEST_ASSERT_EQUAL_UINT(0, virtualBlockMapPtr->block[die][virtualDieMapPtr->die[die].currentBlock].free);
	}
	TEST_ASSERT_TRUE(sliceAllocationTargetDie < USER_DIES);
	TEST_ASSERT_EQUAL_UINT(0, mbPerbadBlockSpace);
}

static void test_init_address_map_hides_bad_block_table_block_behind_reserved_block(void)
{
	unsigned int die;
	for (die = 0; die < USER_DIES; die++)
	{
		unsigned int bbtBlock = bbtInfoMapPtr->bbtInfo[die].phyBlock;
		TEST_ASSERT_EQUAL_UINT(1, phyBlockMapPtr->phyBlock[die][bbtBlock].bad);
		TEST_ASSERT_EQUAL_UINT(FIRST_RESERVED_LUN0, phyBlockMapPtr->phyBlock[die][bbtBlock].remappedPhyBlock);
		/* the remapped block is usable again, so virtual block 0 is not bad */
		TEST_ASSERT_EQUAL_UINT(0, virtualBlockMapPtr->block[die][0].bad);
	}
}

static void test_init_address_map_puts_every_good_block_on_free_list(void)
{
	unsigned int die;
	for (die = 0; die < USER_DIES; die++)
	{
		/* all USER_BLOCKS_PER_DIE are good; one has been taken as current block */
		TEST_ASSERT_EQUAL_UINT(USER_BLOCKS_PER_DIE - 1, virtualDieMapPtr->die[die].freeBlockCnt);
		TEST_ASSERT_EQUAL_UINT(USER_BLOCKS_PER_DIE - 1, fbListLength(die));
		TEST_ASSERT_EQUAL_UINT(BLOCK_NONE, virtualBlockMapPtr->block[die][virtualDieMapPtr->die[die].headFreeBlock].prevBlock);
		TEST_ASSERT_EQUAL_UINT(BLOCK_NONE, virtualBlockMapPtr->block[die][virtualDieMapPtr->die[die].tailFreeBlock].nextBlock);
	}
}

static void test_boot_with_X_erases_total_block_space_before_recovering_table(void)
{
	size_t erases;

	ftl_test_env_reset();
	ftl_test_queue_inbyte("X");
	InitFTL();

	erases = mock_nsc_count_op(MOCK_NSC_OP_ERASE);
	/* total block space (both LUNs incl. extended blocks) + user block space erase */
	TEST_ASSERT_TRUE(erases >= (size_t)TOTAL_BLOCKS_PER_DIE * USER_DIES);
	TEST_ASSERT_TRUE(erases < (size_t)(TOTAL_BLOCKS_PER_DIE + USER_BLOCKS_PER_DIE) * USER_DIES);
}

static void test_boot_without_X_erases_user_block_space_plus_one_bbt_block_per_die(void)
{
	/* erased NAND holds no bad-block table, so boot scans, then erases and
	 * programs one table block per die on top of the user block space erase */
	size_t erases = mock_nsc_count_op(MOCK_NSC_OP_ERASE);
	TEST_ASSERT_EQUAL_size_t((size_t)USER_BLOCKS_PER_DIE * USER_DIES + USER_DIES, erases);
}

/* ------------------------------------------------- AddrTransRead / Write */

static void test_addr_trans_read_returns_fail_for_unmapped_slice(void)
{
	TEST_ASSERT_EQUAL_HEX32(VSA_FAIL, AddrTransRead(0));
	TEST_ASSERT_EQUAL_HEX32(VSA_FAIL, AddrTransRead(SLICES_PER_SSD - 1));
}

static void test_addr_trans_read_returns_mapped_vsa(void)
{
	unsigned int lsa = 4242, vsa = Vorg2VsaTranslation(3, 17, 5);
	logicalSliceMapPtr->logicalSlice[lsa].virtualSliceAddr = vsa;
	TEST_ASSERT_EQUAL_UINT(vsa, AddrTransRead(lsa));
}

static void test_addr_trans_read_asserts_on_out_of_range_lsa(void)
{
	FTL_TEST_EXPECT_ASSERT(AddrTransRead(SLICES_PER_SSD));
}

static void test_addr_trans_write_asserts_on_out_of_range_lsa(void)
{
	FTL_TEST_EXPECT_ASSERT(AddrTransWrite(SLICES_PER_SSD));
}

static void test_addr_trans_write_creates_bidirectional_mapping(void)
{
	unsigned int lsa = 99, vsa = AddrTransWrite(lsa);

	TEST_ASSERT_NOT_EQUAL(VSA_FAIL, vsa);
	TEST_ASSERT_EQUAL_UINT(vsa, logicalSliceMapPtr->logicalSlice[lsa].virtualSliceAddr);
	TEST_ASSERT_EQUAL_UINT(lsa, virtualSliceMapPtr->virtualSlice[vsa].logicalSliceAddr);
	TEST_ASSERT_EQUAL_UINT(vsa, AddrTransRead(lsa));
}

static void test_addr_trans_write_spreads_consecutive_slices_over_dies(void)
{
	unsigned int i, firstDie = Vsa2VdieTranslation(AddrTransWrite(0));
	unsigned int seenDies = 1;
	for (i = 1; i < USER_DIES; i++)
		if (Vsa2VdieTranslation(AddrTransWrite(i)) != firstDie)
			seenDies++;
	TEST_ASSERT_EQUAL_UINT(USER_DIES, seenDies);
}

static void test_addr_trans_write_uses_consecutive_pages_of_current_block(void)
{
	unsigned int die = sliceAllocationTargetDie;
	unsigned int block = virtualDieMapPtr->die[die].currentBlock;
	unsigned int page = virtualBlockMapPtr->block[die][block].currentPage;
	unsigned int vsa = AddrTransWrite(7);

	TEST_ASSERT_EQUAL_UINT(Vorg2VsaTranslation(die, block, page), vsa);
	TEST_ASSERT_EQUAL_UINT(page + 1, virtualBlockMapPtr->block[die][block].currentPage);
}

static void test_rewriting_a_slice_invalidates_old_vsa_and_allocates_new_one(void)
{
	unsigned int lsa = 5, first, second, die, block;

	first = AddrTransWrite(lsa);
	die = Vsa2VdieTranslation(first);
	block = Vsa2VblockTranslation(first);
	TEST_ASSERT_EQUAL_UINT(0, virtualBlockMapPtr->block[die][block].invalidSliceCnt);

	second = AddrTransWrite(lsa);

	TEST_ASSERT_NOT_EQUAL(first, second);
	TEST_ASSERT_EQUAL_UINT(second, AddrTransRead(lsa));
	TEST_ASSERT_EQUAL_UINT(1, virtualBlockMapPtr->block[die][block].invalidSliceCnt);
	/* the stale reverse mapping is left in place; the forward map decides validity */
	TEST_ASSERT_EQUAL_UINT(lsa, virtualSliceMapPtr->virtualSlice[first].logicalSliceAddr);
}

/* --------------------------------------------------------- InvalidateOldVsa */

static void test_invalidate_old_vsa_is_noop_for_unmapped_slice(void)
{
	InvalidateOldVsa(11);
	TEST_ASSERT_EQUAL_HEX32(VSA_NONE, logicalSliceMapPtr->logicalSlice[11].virtualSliceAddr);
}

static void test_invalidate_old_vsa_ignores_slice_whose_reverse_map_points_elsewhere(void)
{
	unsigned int lsa = 20, vsa = AddrTransWrite(lsa);
	unsigned int die = Vsa2VdieTranslation(vsa), block = Vsa2VblockTranslation(vsa);

	virtualSliceMapPtr->virtualSlice[vsa].logicalSliceAddr = lsa + 1;
	InvalidateOldVsa(lsa);

	TEST_ASSERT_EQUAL_UINT(vsa, logicalSliceMapPtr->logicalSlice[lsa].virtualSliceAddr);
	TEST_ASSERT_EQUAL_UINT(0, virtualBlockMapPtr->block[die][block].invalidSliceCnt);
}

static void test_invalidate_old_vsa_moves_block_to_matching_gc_victim_list(void)
{
	unsigned int lsa = 30, vsa = AddrTransWrite(lsa);
	unsigned int die = Vsa2VdieTranslation(vsa), block = Vsa2VblockTranslation(vsa);

	InvalidateOldVsa(lsa);

	TEST_ASSERT_EQUAL_HEX32(VSA_NONE, logicalSliceMapPtr->logicalSlice[lsa].virtualSliceAddr);
	TEST_ASSERT_EQUAL_UINT(1, virtualBlockMapPtr->block[die][block].invalidSliceCnt);
	TEST_ASSERT_EQUAL_UINT(block, gcVictimMapPtr->gcVictimList[die][1].headBlock);
	TEST_ASSERT_EQUAL_UINT(block, gcVictimMapPtr->gcVictimList[die][1].tailBlock);
	TEST_ASSERT_EQUAL_HEX32(VSA_FAIL, AddrTransRead(lsa));
}

/* ------------------------------------------------------ FindFreeVirtualSlice */

static void test_find_free_virtual_slice_rotates_target_die_channel_first(void)
{
	unsigned int die0 = sliceAllocationTargetDie, vsa0 = FindFreeVirtualSlice();
	unsigned int die1 = sliceAllocationTargetDie, vsa1 = FindFreeVirtualSlice();

	TEST_ASSERT_EQUAL_UINT(die0, Vsa2VdieTranslation(vsa0));
	TEST_ASSERT_EQUAL_UINT(die1, Vsa2VdieTranslation(vsa1));
	TEST_ASSERT_EQUAL_UINT((Vdie2PchTranslation(die0) + 1) % USER_CHANNELS, Vdie2PchTranslation(die1));
	if (Vdie2PchTranslation(die0) == USER_CHANNELS - 1)
		TEST_ASSERT_EQUAL_UINT((Vdie2PwayTranslation(die0) + 1) % USER_WAYS, Vdie2PwayTranslation(die1));
	else
		TEST_ASSERT_EQUAL_UINT(Vdie2PwayTranslation(die0), Vdie2PwayTranslation(die1));
}

static void test_find_free_virtual_slice_takes_new_block_when_current_is_full(void)
{
	unsigned int die = sliceAllocationTargetDie;
	unsigned int oldBlock = virtualDieMapPtr->die[die].currentBlock;
	unsigned int freeBefore = virtualDieMapPtr->die[die].freeBlockCnt;
	unsigned int vsa;

	virtualBlockMapPtr->block[die][oldBlock].currentPage = USER_PAGES_PER_BLOCK;
	vsa = FindFreeVirtualSlice();

	TEST_ASSERT_NOT_EQUAL(oldBlock, virtualDieMapPtr->die[die].currentBlock);
	TEST_ASSERT_EQUAL_UINT(Vorg2VsaTranslation(die, virtualDieMapPtr->die[die].currentBlock, 0), vsa);
	TEST_ASSERT_EQUAL_UINT(freeBefore - 1, virtualDieMapPtr->die[die].freeBlockCnt);
	TEST_ASSERT_EQUAL_UINT(1, virtualBlockMapPtr->block[die][virtualDieMapPtr->die[die].currentBlock].currentPage);
}

static void test_find_free_virtual_slice_runs_gc_when_free_list_is_exhausted(void)
{
	unsigned int die = sliceAllocationTargetDie;
	unsigned int current = virtualDieMapPtr->die[die].currentBlock;
	unsigned int victim, vsa;
	size_t erasesBefore;

	drainFreeBlocks(die);
	victim = (current + 1) % USER_BLOCKS_PER_DIE;
	/* victim is fully invalid; currentPage stays 0 so the erase passes the
	 * row-address dependency check without any preceding program requests */
	virtualBlockMapPtr->block[die][victim].invalidSliceCnt = SLICES_PER_BLOCK;
	PutToGcVictimList(die, victim, SLICES_PER_BLOCK);
	virtualBlockMapPtr->block[die][current].currentPage = USER_PAGES_PER_BLOCK;
	erasesBefore = mock_nsc_count_op(MOCK_NSC_OP_ERASE);

	vsa = FindFreeVirtualSlice();
	SyncAllLowLevelReqDone();

	TEST_ASSERT_EQUAL_UINT(die, Vsa2VdieTranslation(vsa));
	TEST_ASSERT_NOT_EQUAL(current, Vsa2VblockTranslation(vsa));
	TEST_ASSERT_EQUAL_UINT(1, virtualBlockMapPtr->block[die][victim].eraseCnt);
	TEST_ASSERT_EQUAL_UINT(0, virtualBlockMapPtr->block[die][victim].invalidSliceCnt);
	TEST_ASSERT_EQUAL_size_t(erasesBefore + 1, mock_nsc_count_op(MOCK_NSC_OP_ERASE));
}

static void test_find_free_virtual_slice_asserts_when_current_page_overflows(void)
{
	unsigned int die = sliceAllocationTargetDie;
	unsigned int block = virtualDieMapPtr->die[die].currentBlock;

	virtualBlockMapPtr->block[die][block].currentPage = USER_PAGES_PER_BLOCK + 1;
	FTL_TEST_EXPECT_ASSERT(FindFreeVirtualSlice());
}

static void test_find_free_virtual_slice_for_gc_reuses_current_block(void)
{
	unsigned int die = 4, block = virtualDieMapPtr->die[die].currentBlock;
	unsigned int page = virtualBlockMapPtr->block[die][block].currentPage;
	unsigned int vsa = FindFreeVirtualSliceForGc(die, block + 1);

	TEST_ASSERT_EQUAL_UINT(Vorg2VsaTranslation(die, block, page), vsa);
	TEST_ASSERT_EQUAL_UINT(block, virtualDieMapPtr->die[die].currentBlock);
}

static void test_find_free_virtual_slice_for_gc_leaves_victim_block(void)
{
	unsigned int die = 4, victim = virtualDieMapPtr->die[die].currentBlock;
	unsigned int vsa = FindFreeVirtualSliceForGc(die, victim);

	TEST_ASSERT_NOT_EQUAL(victim, virtualDieMapPtr->die[die].currentBlock);
	TEST_ASSERT_EQUAL_UINT(Vorg2VsaTranslation(die, virtualDieMapPtr->die[die].currentBlock, 0), vsa);
}

static void test_find_free_virtual_slice_for_gc_can_use_reserved_free_block(void)
{
	unsigned int die = 4, block, vsa;

	drainFreeBlocks(die);
	block = virtualDieMapPtr->die[die].currentBlock;
	virtualBlockMapPtr->block[die][block].currentPage = USER_PAGES_PER_BLOCK;

	vsa = FindFreeVirtualSliceForGc(die, block + 1);

	TEST_ASSERT_NOT_EQUAL(block, virtualDieMapPtr->die[die].currentBlock);
	TEST_ASSERT_EQUAL_UINT(0, virtualDieMapPtr->die[die].freeBlockCnt);
	TEST_ASSERT_EQUAL_UINT(die, Vsa2VdieTranslation(vsa));
}

static void test_find_free_virtual_slice_for_gc_asserts_without_any_free_block(void)
{
	unsigned int die = 4, block;

	drainFreeBlocks(die);
	TEST_ASSERT_NOT_EQUAL(BLOCK_FAIL, GetFromFbList(die, GET_FREE_BLOCK_GC));
	block = virtualDieMapPtr->die[die].currentBlock;
	virtualBlockMapPtr->block[die][block].currentPage = USER_PAGES_PER_BLOCK;

	FTL_TEST_EXPECT_ASSERT(FindFreeVirtualSliceForGc(die, block + 1));
}

static void test_find_free_virtual_slice_for_gc_asserts_when_current_page_overflows(void)
{
	unsigned int die = 4, block = virtualDieMapPtr->die[die].currentBlock;
	virtualBlockMapPtr->block[die][block].currentPage = USER_PAGES_PER_BLOCK + 1;
	FTL_TEST_EXPECT_ASSERT(FindFreeVirtualSliceForGc(die, block + 1));
}

/* ------------------------------------------------------------ free block list */

static void test_put_to_fb_list_appends_at_tail(void)
{
	unsigned int die = 2, tail = virtualDieMapPtr->die[die].tailFreeBlock;
	unsigned int block = virtualDieMapPtr->die[die].currentBlock;
	unsigned int cnt = virtualDieMapPtr->die[die].freeBlockCnt;

	PutToFbList(die, block);

	TEST_ASSERT_EQUAL_UINT(block, virtualDieMapPtr->die[die].tailFreeBlock);
	TEST_ASSERT_EQUAL_UINT(tail, virtualBlockMapPtr->block[die][block].prevBlock);
	TEST_ASSERT_EQUAL_UINT(BLOCK_NONE, virtualBlockMapPtr->block[die][block].nextBlock);
	TEST_ASSERT_EQUAL_UINT(block, virtualBlockMapPtr->block[die][tail].nextBlock);
	TEST_ASSERT_EQUAL_UINT(cnt + 1, virtualDieMapPtr->die[die].freeBlockCnt);
}

static void test_put_to_fb_list_on_empty_list_sets_head_and_tail(void)
{
	unsigned int die = 2;

	InitDieMap();
	TEST_ASSERT_EQUAL_UINT(BLOCK_NONE, virtualDieMapPtr->die[die].headFreeBlock);
	TEST_ASSERT_EQUAL_UINT(0, virtualDieMapPtr->die[die].freeBlockCnt);

	PutToFbList(die, 77);

	TEST_ASSERT_EQUAL_UINT(77, virtualDieMapPtr->die[die].headFreeBlock);
	TEST_ASSERT_EQUAL_UINT(77, virtualDieMapPtr->die[die].tailFreeBlock);
	TEST_ASSERT_EQUAL_UINT(BLOCK_NONE, virtualBlockMapPtr->block[die][77].prevBlock);
	TEST_ASSERT_EQUAL_UINT(BLOCK_NONE, virtualBlockMapPtr->block[die][77].nextBlock);
	TEST_ASSERT_EQUAL_UINT(1, virtualDieMapPtr->die[die].freeBlockCnt);
}

static void test_get_from_fb_list_pops_head_and_marks_block_used(void)
{
	unsigned int die = 2, head = virtualDieMapPtr->die[die].headFreeBlock;
	unsigned int next = virtualBlockMapPtr->block[die][head].nextBlock;
	unsigned int cnt = virtualDieMapPtr->die[die].freeBlockCnt;

	TEST_ASSERT_EQUAL_UINT(head, GetFromFbList(die, GET_FREE_BLOCK_NORMAL));
	TEST_ASSERT_EQUAL_UINT(next, virtualDieMapPtr->die[die].headFreeBlock);
	TEST_ASSERT_EQUAL_UINT(BLOCK_NONE, virtualBlockMapPtr->block[die][next].prevBlock);
	TEST_ASSERT_EQUAL_UINT(0, virtualBlockMapPtr->block[die][head].free);
	TEST_ASSERT_EQUAL_UINT(BLOCK_NONE, virtualBlockMapPtr->block[die][head].nextBlock);
	TEST_ASSERT_EQUAL_UINT(cnt - 1, virtualDieMapPtr->die[die].freeBlockCnt);
}

static void test_get_from_fb_list_normal_keeps_reserved_blocks(void)
{
	unsigned int die = 2;
	drainFreeBlocks(die);
	TEST_ASSERT_EQUAL_UINT(BLOCK_FAIL, GetFromFbList(die, GET_FREE_BLOCK_NORMAL));
	TEST_ASSERT_EQUAL_UINT(RESERVED_FREE_BLOCK_COUNT, fbListLength(die));
}

static void test_get_from_fb_list_gc_takes_last_block_and_empties_list(void)
{
	unsigned int die = 2, last;

	drainFreeBlocks(die);
	last = virtualDieMapPtr->die[die].headFreeBlock;

	TEST_ASSERT_EQUAL_UINT(last, GetFromFbList(die, GET_FREE_BLOCK_GC));
	TEST_ASSERT_EQUAL_UINT(BLOCK_NONE, virtualDieMapPtr->die[die].headFreeBlock);
	TEST_ASSERT_EQUAL_UINT(BLOCK_NONE, virtualDieMapPtr->die[die].tailFreeBlock);
	TEST_ASSERT_EQUAL_UINT(0, virtualDieMapPtr->die[die].freeBlockCnt);
	TEST_ASSERT_EQUAL_UINT(BLOCK_FAIL, GetFromFbList(die, GET_FREE_BLOCK_GC));
}

static void test_get_from_fb_list_asserts_on_unknown_option(void)
{
	FTL_TEST_EXPECT_ASSERT(GetFromFbList(2, GET_FREE_BLOCK_GC + 1));
}

static void test_fb_list_round_trips_a_block(void)
{
	unsigned int die = 2, block = GetFromFbList(die, GET_FREE_BLOCK_NORMAL);
	unsigned int len = fbListLength(die);

	PutToFbList(die, block);
	TEST_ASSERT_EQUAL_UINT(len + 1, fbListLength(die));
	TEST_ASSERT_EQUAL_UINT(block, virtualDieMapPtr->die[die].tailFreeBlock);
}

/* ------------------------------------------------------------------ EraseBlock */

static void test_erase_block_recycles_block_and_clears_slice_mappings(void)
{
	/* a block with no outstanding program requests, so the erase is not held
	 * back by the row-address dependency check; mappings are set by hand */
	unsigned int die = 2, block = GetFromFbList(die, GET_FREE_BLOCK_NORMAL);
	unsigned int lsa = 8, vsa = Vorg2VsaTranslation(die, block, 3);
	unsigned int cnt = virtualDieMapPtr->die[die].freeBlockCnt;
	size_t erasesBefore = mock_nsc_count_op(MOCK_NSC_OP_ERASE);

	logicalSliceMapPtr->logicalSlice[lsa].virtualSliceAddr = vsa;
	virtualSliceMapPtr->virtualSlice[vsa].logicalSliceAddr = lsa;
	virtualBlockMapPtr->block[die][block].invalidSliceCnt = 4;

	EraseBlock(die, block);
	SyncAllLowLevelReqDone();

	TEST_ASSERT_EQUAL_UINT(1, virtualBlockMapPtr->block[die][block].free);
	TEST_ASSERT_EQUAL_UINT(1, virtualBlockMapPtr->block[die][block].eraseCnt);
	TEST_ASSERT_EQUAL_UINT(0, virtualBlockMapPtr->block[die][block].currentPage);
	TEST_ASSERT_EQUAL_UINT(0, virtualBlockMapPtr->block[die][block].invalidSliceCnt);
	TEST_ASSERT_EQUAL_UINT(cnt + 1, virtualDieMapPtr->die[die].freeBlockCnt);
	TEST_ASSERT_EQUAL_UINT(block, virtualDieMapPtr->die[die].tailFreeBlock);
	TEST_ASSERT_EQUAL_HEX32(LSA_NONE, virtualSliceMapPtr->virtualSlice[vsa].logicalSliceAddr);
	/* EraseBlock only clears the reverse map; GC must have migrated (or
	 * invalidated) every valid slice beforehand, so the forward map is left as is */
	TEST_ASSERT_EQUAL_UINT(vsa, logicalSliceMapPtr->logicalSlice[lsa].virtualSliceAddr);
	TEST_ASSERT_EQUAL_size_t(erasesBefore + 1, mock_nsc_count_op(MOCK_NSC_OP_ERASE));
}

/* --------------------------------------------------------------- RemapBadBlock */

static void test_remap_bad_block_uses_next_reserved_block_of_same_lun(void)
{
	unsigned int die = 0;

	resetPhyBlockMapOfDie(die);
	markBad(die, 5);
	markBad(die, 9);
	markBad(die, TOTAL_BLOCKS_PER_LUN + 7);

	RemapBadBlock();

	TEST_ASSERT_EQUAL_UINT(FIRST_RESERVED_LUN0, phyBlockMapPtr->phyBlock[die][5].remappedPhyBlock);
	TEST_ASSERT_EQUAL_UINT(FIRST_RESERVED_LUN0 + 1, phyBlockMapPtr->phyBlock[die][9].remappedPhyBlock);
	TEST_ASSERT_EQUAL_UINT(FIRST_RESERVED_LUN1, phyBlockMapPtr->phyBlock[die][TOTAL_BLOCKS_PER_LUN + 7].remappedPhyBlock);
	TEST_ASSERT_EQUAL_UINT(6, phyBlockMapPtr->phyBlock[die][6].remappedPhyBlock);
	TEST_ASSERT_EQUAL_UINT(0, mbPerbadBlockSpace);
}

static void test_remap_bad_block_skips_bad_reserved_blocks(void)
{
	unsigned int die = 3;

	resetPhyBlockMapOfDie(die);
	markBad(die, 1);
	markBad(die, FIRST_RESERVED_LUN0);
	markBad(die, FIRST_RESERVED_LUN0 + 1);
	markBad(die, TOTAL_BLOCKS_PER_LUN + 1);
	markBad(die, FIRST_RESERVED_LUN1);

	RemapBadBlock();

	TEST_ASSERT_EQUAL_UINT(FIRST_RESERVED_LUN0 + 2, phyBlockMapPtr->phyBlock[die][1].remappedPhyBlock);
	TEST_ASSERT_EQUAL_UINT(FIRST_RESERVED_LUN1 + 1, phyBlockMapPtr->phyBlock[die][TOTAL_BLOCKS_PER_LUN + 1].remappedPhyBlock);
	TEST_ASSERT_EQUAL_UINT(0, mbPerbadBlockSpace);
}

static void test_remap_bad_block_counts_unremappable_blocks_of_lun0(void)
{
	unsigned int die = 1, b;

	resetPhyBlockMapOfDie(die);
	for (b = FIRST_RESERVED_LUN0; b < TOTAL_BLOCKS_PER_LUN; b++)
		markBad(die, b);
	markBad(die, 3);   /* exhausts the reserved area while scanning */
	markBad(die, 10);  /* reserved area already exhausted */

	RemapBadBlock();

	TEST_ASSERT_EQUAL_UINT(3, phyBlockMapPtr->phyBlock[die][3].remappedPhyBlock);
	TEST_ASSERT_EQUAL_UINT(10, phyBlockMapPtr->phyBlock[die][10].remappedPhyBlock);
	TEST_ASSERT_EQUAL_UINT(2 * USER_DIES * MB_PER_BLOCK, mbPerbadBlockSpace);
}

static void test_remap_bad_block_counts_unremappable_blocks_of_lun1(void)
{
	unsigned int die = 1, b;

	resetPhyBlockMapOfDie(die);
	for (b = FIRST_RESERVED_LUN1; b < TOTAL_BLOCKS_PER_DIE; b++)
		markBad(die, b);
	markBad(die, TOTAL_BLOCKS_PER_LUN + 3);
	markBad(die, TOTAL_BLOCKS_PER_LUN + 10);
	markBad(die, TOTAL_BLOCKS_PER_LUN + 11);

	RemapBadBlock();

	TEST_ASSERT_EQUAL_UINT(TOTAL_BLOCKS_PER_LUN + 3, phyBlockMapPtr->phyBlock[die][TOTAL_BLOCKS_PER_LUN + 3].remappedPhyBlock);
	TEST_ASSERT_EQUAL_UINT(TOTAL_BLOCKS_PER_LUN + 10, phyBlockMapPtr->phyBlock[die][TOTAL_BLOCKS_PER_LUN + 10].remappedPhyBlock);
	TEST_ASSERT_EQUAL_UINT(3 * USER_DIES * MB_PER_BLOCK, mbPerbadBlockSpace);
}

static void test_remap_bad_block_reports_worst_die(void)
{
	unsigned int b;

	resetPhyBlockMapOfDie(5);
	resetPhyBlockMapOfDie(6);
	for (b = FIRST_RESERVED_LUN0; b < TOTAL_BLOCKS_PER_LUN; b++)
	{
		markBad(5, b);
		markBad(6, b);
	}
	markBad(5, 0);
	markBad(6, 0);
	markBad(6, 1);
	markBad(6, 2);
	markBad(6, 3);

	RemapBadBlock();

	TEST_ASSERT_EQUAL_UINT(4 * USER_DIES * MB_PER_BLOCK, mbPerbadBlockSpace);
}

static void test_init_block_map_excludes_bad_virtual_blocks_from_free_list(void)
{
	unsigned int die = 0;

	resetPhyBlockMapOfDie(die);
	markBad(die, 12);
	markBad(die, FIRST_RESERVED_LUN0);
	InitDieMap();
	InitBlockMap();

	TEST_ASSERT_EQUAL_UINT(1, virtualBlockMapPtr->block[die][12].bad);
	TEST_ASSERT_EQUAL_UINT(BLOCK_NONE, virtualBlockMapPtr->block[die][12].prevBlock);
	TEST_ASSERT_EQUAL_UINT(BLOCK_NONE, virtualBlockMapPtr->block[die][12].nextBlock);
	TEST_ASSERT_EQUAL_UINT(USER_BLOCKS_PER_DIE - 1, virtualDieMapPtr->die[die].freeBlockCnt);
	TEST_ASSERT_EQUAL_UINT(USER_BLOCKS_PER_DIE, virtualDieMapPtr->die[1].freeBlockCnt);
}

static void test_init_block_map_follows_remapping_to_decide_badness(void)
{
	unsigned int die = 0;

	resetPhyBlockMapOfDie(die);
	markBad(die, 12);
	RemapBadBlock();
	InitDieMap();
	InitBlockMap();

	TEST_ASSERT_EQUAL_UINT(0, virtualBlockMapPtr->block[die][12].bad);
	TEST_ASSERT_EQUAL_UINT(USER_BLOCKS_PER_DIE, virtualDieMapPtr->die[die].freeBlockCnt);
}

static void test_init_current_block_asserts_when_a_die_has_no_free_block(void)
{
	InitDieMap();
	FTL_TEST_EXPECT_ASSERT(InitCurrentBlockOfDieMap());
}

/* ------------------------------------------------------ RecoverBadBlockTable */

/* NAND row address of a page in total-block-space numbering (both LUNs). */
static unsigned int rowOfPhyBlockPage(unsigned int phyBlockNo, unsigned int pageNo)
{
	unsigned int lunBase = (phyBlockNo / TOTAL_BLOCKS_PER_LUN) ? LUN_1_BASE_ADDR : LUN_0_BASE_ADDR;
	return lunBase + (phyBlockNo % TOTAL_BLOCKS_PER_LUN) * PAGES_PER_MLC_BLOCK + pageNo;
}

static void test_recover_bbt_with_existing_table_restores_bad_marks(void)
{
	unsigned int die = 0, b;

	memset(nandPage, BLOCK_STATE_NORMAL, sizeof(nandPage));
	nandPage[42] = BLOCK_STATE_BAD;
	nandPage[TOTAL_BLOCKS_PER_DIE - 1] = BLOCK_STATE_BAD;
	mock_nsc_set_read_page_source(chCtlReg[0], 0, nandPage, sizeof(nandPage));
	resetPhyBlockMapOfDie(die);
	bbtInfoMapPtr->bbtInfo[die].grownBadUpdate = BBT_INFO_GROWN_BAD_UPDATE_BOOKED;
	size_t programsBefore = mock_nsc_count_op(MOCK_NSC_OP_PROGRAM);

	RecoverBadBlockTable(RESERVED_DATA_BUFFER_BASE_ADDR);

	TEST_ASSERT_EQUAL_UINT(1, phyBlockMapPtr->phyBlock[die][42].bad);
	TEST_ASSERT_EQUAL_UINT(1, phyBlockMapPtr->phyBlock[die][TOTAL_BLOCKS_PER_DIE - 1].bad);
	for (b = 0; b < TOTAL_BLOCKS_PER_DIE; b++)
		if (b != 42 && b != TOTAL_BLOCKS_PER_DIE - 1)
			TEST_ASSERT_EQUAL_UINT(0, phyBlockMapPtr->phyBlock[die][b].bad);
	TEST_ASSERT_EQUAL_UINT(BBT_INFO_GROWN_BAD_UPDATE_NONE, bbtInfoMapPtr->bbtInfo[die].grownBadUpdate);
	/* no table had to be rebuilt, so nothing was written to flash */
	TEST_ASSERT_EQUAL_size_t(programsBefore, mock_nsc_count_op(MOCK_NSC_OP_PROGRAM));
}

static void test_recover_bbt_reads_table_pages_from_bbt_block(void)
{
	size_t i, n, seen = 0;

	/* serve a valid table so no bad-block scan follows the table read */
	memset(nandPage, BLOCK_STATE_NORMAL, sizeof(nandPage));
	mock_nsc_set_read_page_source(chCtlReg[0], 0, nandPage, sizeof(nandPage));
	bbtInfoMapPtr->bbtInfo[0].phyBlock = 3;
	n = mock_nsc_call_count();
	RecoverBadBlockTable(RESERVED_DATA_BUFFER_BASE_ADDR);

	for (i = n; i < mock_nsc_call_count(); i++)
	{
		const mock_nsc_call_t *c = mock_nsc_call_at(i);
		if (c->op == MOCK_NSC_OP_READ_TRIGGER && c->dev == chCtlReg[0] && c->way == 0)
		{
			TEST_ASSERT_EQUAL_UINT(rowOfPhyBlockPage(3, Vpage2PlsbPageTranslation(1)), c->rowAddress);
			seen++;
		}
	}
	TEST_ASSERT_EQUAL_size_t(USED_PAGES_FOR_BAD_BLOCK_TABLE_PER_DIE, seen);
}

/* Wipe the bad-block table that boot programmed into die 0, so the table
 * read returns erased NAND and RecoverBadBlockTable() must rebuild it. */
static void eraseDie0BadBlockTable(void)
{
	V2FEraseBlockAsync(chCtlReg[0], 0, rowOfPhyBlockPage(bbtInfoMapPtr->bbtInfo[0].phyBlock, 0));
}

static void test_recover_bbt_without_table_scans_marks_and_saves_new_table(void)
{
	unsigned int die = 0, b;
	size_t programsBefore, erasesBefore;

	eraseDie0BadBlockTable();
	resetPhyBlockMapOfDie(die);
	markBad(die, 1); /* stale entry that the scan must overwrite */
	markBad(1, 7);   /* stale entry that die 1's stored table must overwrite */
	programsBefore = mock_nsc_count_op(MOCK_NSC_OP_PROGRAM);
	erasesBefore = mock_nsc_count_op(MOCK_NSC_OP_ERASE);

	RecoverBadBlockTable(RESERVED_DATA_BUFFER_BASE_ADDR);

	for (b = 0; b < TOTAL_BLOCKS_PER_DIE; b++)
	{
		TEST_ASSERT_EQUAL_UINT(0, phyBlockMapPtr->phyBlock[die][b].bad);
		TEST_ASSERT_EQUAL_UINT8(BLOCK_STATE_NORMAL, bbtBufOfDie(die)[b]);
	}
	TEST_ASSERT_EQUAL_size_t(erasesBefore + 1, mock_nsc_count_op(MOCK_NSC_OP_ERASE));
	TEST_ASSERT_EQUAL_size_t(programsBefore + USED_PAGES_FOR_BAD_BLOCK_TABLE_PER_DIE,
	                         mock_nsc_count_op(MOCK_NSC_OP_PROGRAM));
	/* dies with an existing table are restored from flash, not rescanned */
	TEST_ASSERT_EQUAL_UINT(0, phyBlockMapPtr->phyBlock[1][7].bad);
}

static void test_recover_bbt_without_table_detects_mark_in_first_page(void)
{
	unsigned int die = 0, b;

	setDie0ReadPage(CLEAN_DATA_IN_BYTE, BBT_MARK_CLEAN, BBT_MARK_BAD);
	resetPhyBlockMapOfDie(die);

	RecoverBadBlockTable(RESERVED_DATA_BUFFER_BASE_ADDR);

	for (b = 0; b < TOTAL_BLOCKS_PER_DIE; b++)
	{
		TEST_ASSERT_EQUAL_UINT(1, phyBlockMapPtr->phyBlock[die][b].bad);
		TEST_ASSERT_EQUAL_UINT8(BLOCK_STATE_BAD, bbtBufOfDie(die)[b]);
	}
	TEST_ASSERT_EQUAL_UINT(0, phyBlockMapPtr->phyBlock[1][5].bad);
}

static void test_recover_bbt_without_table_detects_mark_in_last_page(void)
{
	unsigned int die = 0, b;

	/* first-page marks stay clean; only the last row of odd blocks is marked */
	eraseDie0BadBlockTable();
	memset(nandPage, CLEAN_DATA_IN_BYTE, sizeof(nandPage));
	nandPage[BAD_BLOCK_MARK_BYTE0] = BBT_MARK_BAD;
	for (b = 1; b < TOTAL_BLOCKS_PER_DIE; b += 2)
		V2FProgramPageAsync(chCtlReg[0], 0, rowOfPhyBlockPage(b, BAD_BLOCK_MARK_PAGE1), nandPage, NULL);
	resetPhyBlockMapOfDie(die);

	RecoverBadBlockTable(RESERVED_DATA_BUFFER_BASE_ADDR);

	for (b = 0; b < TOTAL_BLOCKS_PER_DIE; b++)
		TEST_ASSERT_EQUAL_UINT(b & 1, phyBlockMapPtr->phyBlock[die][b].bad);
}

static void test_full_boot_with_all_blocks_bad_in_one_die_asserts(void)
{
	setDie0ReadPage(CLEAN_DATA_IN_BYTE, BBT_MARK_BAD, BBT_MARK_BAD);
	FTL_TEST_EXPECT_ASSERT(InitAddressMap());

	TEST_ASSERT_EQUAL_UINT(1, virtualBlockMapPtr->block[0][0].bad);
	TEST_ASSERT_EQUAL_UINT(0, virtualDieMapPtr->die[0].freeBlockCnt);
	TEST_ASSERT_EQUAL_UINT(USER_BLOCKS_PER_DIE * USER_DIES * MB_PER_BLOCK, mbPerbadBlockSpace);
}

/* ------------------------------------------------------- grown bad blocks */

static void test_update_phy_block_map_for_grown_bad_block_books_table_update(void)
{
	UpdatePhyBlockMapForGrownBadBlock(6, 1234);

	TEST_ASSERT_EQUAL_UINT(BLOCK_STATE_BAD, phyBlockMapPtr->phyBlock[6][1234].bad);
	TEST_ASSERT_EQUAL_UINT(BBT_INFO_GROWN_BAD_UPDATE_BOOKED, bbtInfoMapPtr->bbtInfo[6].grownBadUpdate);
	TEST_ASSERT_EQUAL_UINT(BBT_INFO_GROWN_BAD_UPDATE_NONE, bbtInfoMapPtr->bbtInfo[5].grownBadUpdate);
}

static void test_update_bbt_for_grown_bad_block_rewrites_only_booked_dies(void)
{
	unsigned int die = 6, bbtBlock = bbtInfoMapPtr->bbtInfo[die].phyBlock;
	size_t programsBefore = mock_nsc_count_op(MOCK_NSC_OP_PROGRAM);
	size_t erasesBefore = mock_nsc_count_op(MOCK_NSC_OP_ERASE);
	size_t i, n = mock_nsc_call_count();
	const unsigned char *stored;

	UpdatePhyBlockMapForGrownBadBlock(die, 1234);
	UpdateBadBlockTableForGrownBadBlock(RESERVED_DATA_BUFFER_BASE_ADDR);

	TEST_ASSERT_EQUAL_UINT8(BLOCK_STATE_BAD, bbtBufOfDie(die)[1234]);
	TEST_ASSERT_EQUAL_UINT8(BLOCK_STATE_NORMAL, bbtBufOfDie(die)[1233]);
	/* the block holding the table itself is never recorded as bad */
	TEST_ASSERT_EQUAL_UINT8(BLOCK_STATE_NORMAL, bbtBufOfDie(die)[bbtBlock]);
	TEST_ASSERT_EQUAL_size_t(erasesBefore + 1, mock_nsc_count_op(MOCK_NSC_OP_ERASE));
	TEST_ASSERT_EQUAL_size_t(programsBefore + USED_PAGES_FOR_BAD_BLOCK_TABLE_PER_DIE,
	                         mock_nsc_count_op(MOCK_NSC_OP_PROGRAM));
	for (i = n; i < mock_nsc_call_count(); i++)
	{
		const mock_nsc_call_t *c = mock_nsc_call_at(i);
		if (c->op == MOCK_NSC_OP_PROGRAM || c->op == MOCK_NSC_OP_ERASE)
		{
			TEST_ASSERT_EQUAL_PTR(chCtlReg[Vdie2PchTranslation(die)], c->dev);
			TEST_ASSERT_EQUAL_INT(Vdie2PwayTranslation(die), c->way);
			TEST_ASSERT_EQUAL_UINT(bbtBlock, c->rowAddress / ROWS_PER_MLC_BLOCK);
		}
	}

	/* the mark must have reached the persistent table page in NAND */
	stored = mock_nsc_page_data(chCtlReg[Vdie2PchTranslation(die)], Vdie2PwayTranslation(die),
	                            rowOfPhyBlockPage(bbtBlock, Vpage2PlsbPageTranslation(1)));
	TEST_ASSERT_NOT_NULL(stored);
	TEST_ASSERT_EQUAL_UINT8(BLOCK_STATE_BAD, stored[1234]);
	TEST_ASSERT_EQUAL_UINT8(BLOCK_STATE_NORMAL, stored[1233]);
	TEST_ASSERT_EQUAL_UINT8(BLOCK_STATE_NORMAL, stored[bbtBlock]);

	/* ... and survives a table recovery from that NAND page */
	resetPhyBlockMapOfDie(die);
	RecoverBadBlockTable(RESERVED_DATA_BUFFER_BASE_ADDR);
	TEST_ASSERT_EQUAL_UINT(1, phyBlockMapPtr->phyBlock[die][1234].bad);
	TEST_ASSERT_EQUAL_UINT(0, phyBlockMapPtr->phyBlock[die][1233].bad);
}

int main(void)
{
	UNITY_BEGIN();
	RUN_TEST(test_vsa_to_vorg_macros_invert_vorg_to_vsa);
	RUN_TEST(test_die_to_channel_way_macros_are_channel_interleaved);
	RUN_TEST(test_virtual_block_to_physical_block_skips_extended_blocks_of_lun0);
	RUN_TEST(test_lsb_page_translation_round_trips);

	RUN_TEST(test_init_address_map_leaves_every_logical_slice_unmapped);
	RUN_TEST(test_init_address_map_hides_bad_block_table_block_behind_reserved_block);
	RUN_TEST(test_init_address_map_puts_every_good_block_on_free_list);
	RUN_TEST(test_boot_with_X_erases_total_block_space_before_recovering_table);
	RUN_TEST(test_boot_without_X_erases_user_block_space_plus_one_bbt_block_per_die);

	RUN_TEST(test_addr_trans_read_returns_fail_for_unmapped_slice);
	RUN_TEST(test_addr_trans_read_returns_mapped_vsa);
	RUN_TEST(test_addr_trans_read_asserts_on_out_of_range_lsa);
	RUN_TEST(test_addr_trans_write_asserts_on_out_of_range_lsa);
	RUN_TEST(test_addr_trans_write_creates_bidirectional_mapping);
	RUN_TEST(test_addr_trans_write_spreads_consecutive_slices_over_dies);
	RUN_TEST(test_addr_trans_write_uses_consecutive_pages_of_current_block);
	RUN_TEST(test_rewriting_a_slice_invalidates_old_vsa_and_allocates_new_one);

	RUN_TEST(test_invalidate_old_vsa_is_noop_for_unmapped_slice);
	RUN_TEST(test_invalidate_old_vsa_ignores_slice_whose_reverse_map_points_elsewhere);
	RUN_TEST(test_invalidate_old_vsa_moves_block_to_matching_gc_victim_list);

	RUN_TEST(test_find_free_virtual_slice_rotates_target_die_channel_first);
	RUN_TEST(test_find_free_virtual_slice_takes_new_block_when_current_is_full);
	RUN_TEST(test_find_free_virtual_slice_runs_gc_when_free_list_is_exhausted);
	RUN_TEST(test_find_free_virtual_slice_asserts_when_current_page_overflows);
	RUN_TEST(test_find_free_virtual_slice_for_gc_reuses_current_block);
	RUN_TEST(test_find_free_virtual_slice_for_gc_leaves_victim_block);
	RUN_TEST(test_find_free_virtual_slice_for_gc_can_use_reserved_free_block);
	RUN_TEST(test_find_free_virtual_slice_for_gc_asserts_without_any_free_block);
	RUN_TEST(test_find_free_virtual_slice_for_gc_asserts_when_current_page_overflows);

	RUN_TEST(test_put_to_fb_list_appends_at_tail);
	RUN_TEST(test_put_to_fb_list_on_empty_list_sets_head_and_tail);
	RUN_TEST(test_get_from_fb_list_pops_head_and_marks_block_used);
	RUN_TEST(test_get_from_fb_list_normal_keeps_reserved_blocks);
	RUN_TEST(test_get_from_fb_list_gc_takes_last_block_and_empties_list);
	RUN_TEST(test_get_from_fb_list_asserts_on_unknown_option);
	RUN_TEST(test_fb_list_round_trips_a_block);

	RUN_TEST(test_erase_block_recycles_block_and_clears_slice_mappings);

	RUN_TEST(test_remap_bad_block_uses_next_reserved_block_of_same_lun);
	RUN_TEST(test_remap_bad_block_skips_bad_reserved_blocks);
	RUN_TEST(test_remap_bad_block_counts_unremappable_blocks_of_lun0);
	RUN_TEST(test_remap_bad_block_counts_unremappable_blocks_of_lun1);
	RUN_TEST(test_remap_bad_block_reports_worst_die);
	RUN_TEST(test_init_block_map_excludes_bad_virtual_blocks_from_free_list);
	RUN_TEST(test_init_block_map_follows_remapping_to_decide_badness);
	RUN_TEST(test_init_current_block_asserts_when_a_die_has_no_free_block);

	RUN_TEST(test_recover_bbt_with_existing_table_restores_bad_marks);
	RUN_TEST(test_recover_bbt_reads_table_pages_from_bbt_block);
	RUN_TEST(test_recover_bbt_without_table_scans_marks_and_saves_new_table);
	RUN_TEST(test_recover_bbt_without_table_detects_mark_in_first_page);
	RUN_TEST(test_recover_bbt_without_table_detects_mark_in_last_page);
	RUN_TEST(test_full_boot_with_all_blocks_bad_in_one_die_asserts);

	RUN_TEST(test_update_phy_block_map_for_grown_bad_block_books_table_update);
	RUN_TEST(test_update_bbt_for_grown_bad_block_rewrites_only_booked_dies);
	return UNITY_END();
}
