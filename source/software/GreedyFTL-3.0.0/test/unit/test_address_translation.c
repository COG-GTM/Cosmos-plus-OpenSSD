/* Unit tests for address_translation.c: LBA->VSA mapping, free block list,
 * slice invalidation and block erase. */
#include "unity.h"

#include "fw_test.h"
#include "memory_map.h"

/* File-local firmware functions without a prototype in address_translation.h. */
void EraseTotalBlockSpace(void);
void EraseUserBlockSpace(void);

/* The mock call log is bounded, so track the last erase through a hook. */
static mock_nsc_call_t lastErase;
static unsigned int eraseSeen;

static void record_last_erase(const mock_nsc_call_t *call)
{
	if (call->cmd == V2FCommand_BlockErase) {
		lastErase = *call;
		eraseSeen = 1;
	}
}

void setUp(void)
{
	fw_test_reset();
	fw_test_init_ftl();
}

void tearDown(void) {}

static void test_smoke_unwritten_lsa_translates_to_vsa_fail(void)
{
	TEST_ASSERT_EQUAL_HEX32(VSA_FAIL, AddrTransRead(0));
}

static void test_smoke_write_then_read_maps_lsa_to_same_vsa(void)
{
	const unsigned int lsa = 5;
	unsigned int vsa = AddrTransWrite(lsa);

	TEST_ASSERT_NOT_EQUAL(VSA_FAIL, vsa);
	TEST_ASSERT_EQUAL_HEX32(vsa, AddrTransRead(lsa));
	TEST_ASSERT_EQUAL_UINT(lsa, virtualSliceMapPtr->virtualSlice[vsa].logicalSliceAddr);
}

static void test_read_out_of_range_lsa_asserts(void)
{
	FW_EXPECT_ASSERT(AddrTransRead(SLICES_PER_SSD));
}

static void test_write_out_of_range_lsa_asserts(void)
{
	FW_EXPECT_ASSERT(AddrTransWrite(SLICES_PER_SSD));
}

static void test_last_valid_lsa_is_accepted(void)
{
	const unsigned int lsa = SLICES_PER_SSD - 1;
	unsigned int vsa = AddrTransWrite(lsa);

	TEST_ASSERT_EQUAL_HEX32(vsa, AddrTransRead(lsa));
}

static void test_translation_macros_round_trip_vsa(void)
{
	const unsigned int dieNo = USER_DIES - 1;
	const unsigned int blockNo = 3;
	const unsigned int pageNo = 7;
	unsigned int vsa = Vorg2VsaTranslation(dieNo, blockNo, pageNo);

	TEST_ASSERT_EQUAL_UINT(dieNo, Vsa2VdieTranslation(vsa));
	TEST_ASSERT_EQUAL_UINT(blockNo, Vsa2VblockTranslation(vsa));
	TEST_ASSERT_EQUAL_UINT(pageNo, Vsa2VpageTranslation(vsa));
	TEST_ASSERT_EQUAL_UINT(dieNo % USER_CHANNELS, Vdie2PchTranslation(dieNo));
	TEST_ASSERT_EQUAL_UINT(dieNo / USER_CHANNELS, Vdie2PwayTranslation(dieNo));
	TEST_ASSERT_EQUAL_UINT(dieNo, Pcw2VdieTranslation(Vdie2PchTranslation(dieNo), Vdie2PwayTranslation(dieNo)));
}

static void test_block_space_translation_macros(void)
{
	/* first block of LUN1 skips the extended blocks of LUN0 */
	TEST_ASSERT_EQUAL_UINT(TOTAL_BLOCKS_PER_LUN, Vblock2PblockOfTbsTranslation(USER_BLOCKS_PER_LUN));
	TEST_ASSERT_EQUAL_UINT(MAIN_BLOCKS_PER_LUN, Vblock2PblockOfMbsTranslation(USER_BLOCKS_PER_LUN));
	TEST_ASSERT_EQUAL_UINT(5, Vblock2PblockOfTbsTranslation(5));
	/* SLC lsb page mapping: page 0 -> row 0, page n -> row 2n-1 */
	TEST_ASSERT_EQUAL_UINT(0, Vpage2PlsbPageTranslation(0));
	TEST_ASSERT_EQUAL_UINT(1, Vpage2PlsbPageTranslation(1));
	TEST_ASSERT_EQUAL_UINT(5, Vpage2PlsbPageTranslation(3));
	TEST_ASSERT_EQUAL_UINT(3, PlsbPage2VpageTranslation(5));
	TEST_ASSERT_EQUAL_UINT(0, PlsbPage2VpageTranslation(0));
}

static void test_find_die_round_robins_channels_then_ways(void)
{
	unsigned int i;

	/* fw_test_reset() rewound the cursor; InitAddressMap consumed die 0. */
	for (i = 1; i < USER_DIES; i++)
		TEST_ASSERT_EQUAL_UINT(Pcw2VdieTranslation(i % USER_CHANNELS, i / USER_CHANNELS),
				FindDieForFreeSliceAllocation());
	TEST_ASSERT_EQUAL_UINT(0, FindDieForFreeSliceAllocation());
}

static void test_consecutive_writes_spread_over_dies(void)
{
	unsigned int first = AddrTransWrite(0);
	unsigned int second = AddrTransWrite(1);

	TEST_ASSERT_NOT_EQUAL(Vsa2VdieTranslation(first), Vsa2VdieTranslation(second));
	TEST_ASSERT_EQUAL_UINT((Vsa2VdieTranslation(first) + 1) % USER_DIES, Vsa2VdieTranslation(second));
}

static void test_write_bumps_current_page_of_target_die(void)
{
	unsigned int dieNo = sliceAllocationTargetDie;
	unsigned int blockNo = virtualDieMapPtr->die[dieNo].currentBlock;
	unsigned int before = virtualBlockMapPtr->block[dieNo][blockNo].currentPage;
	unsigned int vsa = AddrTransWrite(9);

	TEST_ASSERT_EQUAL_UINT(dieNo, Vsa2VdieTranslation(vsa));
	TEST_ASSERT_EQUAL_UINT(blockNo, Vsa2VblockTranslation(vsa));
	TEST_ASSERT_EQUAL_UINT(before, Vsa2VpageTranslation(vsa));
	TEST_ASSERT_EQUAL_UINT(before + 1, virtualBlockMapPtr->block[dieNo][blockNo].currentPage);
}

static void test_rewrite_invalidates_old_vsa_and_queues_gc_victim(void)
{
	const unsigned int lsa = 11;
	unsigned int oldVsa = AddrTransWrite(lsa);
	unsigned int dieNo = Vsa2VdieTranslation(oldVsa);
	unsigned int blockNo = Vsa2VblockTranslation(oldVsa);
	unsigned int newVsa;

	TEST_ASSERT_EQUAL_UINT(0, virtualBlockMapPtr->block[dieNo][blockNo].invalidSliceCnt);

	newVsa = AddrTransWrite(lsa);

	TEST_ASSERT_NOT_EQUAL(oldVsa, newVsa);
	TEST_ASSERT_EQUAL_HEX32(newVsa, AddrTransRead(lsa));
	TEST_ASSERT_EQUAL_UINT(1, virtualBlockMapPtr->block[dieNo][blockNo].invalidSliceCnt);
	TEST_ASSERT_EQUAL_UINT(blockNo, gcVictimMapPtr->gcVictimList[dieNo][1].headBlock);
	TEST_ASSERT_EQUAL_UINT(blockNo, GetFromGcVictimList(dieNo));
}

static void test_invalidate_unmapped_lsa_is_noop(void)
{
	InvalidateOldVsa(3);

	TEST_ASSERT_EQUAL_HEX32(VSA_FAIL, AddrTransRead(3));
	TEST_ASSERT_EQUAL_UINT(0, virtualBlockMapPtr->block[0][virtualDieMapPtr->die[0].currentBlock].invalidSliceCnt);
}

static void test_invalidate_skips_vsa_owned_by_other_lsa(void)
{
	const unsigned int lsa = 4;
	unsigned int vsa = AddrTransWrite(lsa);
	unsigned int dieNo = Vsa2VdieTranslation(vsa);
	unsigned int blockNo = Vsa2VblockTranslation(vsa);

	/* stale forward mapping: the slice has been reassigned to another LSA */
	virtualSliceMapPtr->virtualSlice[vsa].logicalSliceAddr = lsa + 1;

	InvalidateOldVsa(lsa);

	TEST_ASSERT_EQUAL_HEX32(vsa, logicalSliceMapPtr->logicalSlice[lsa].virtualSliceAddr);
	TEST_ASSERT_EQUAL_UINT(0, virtualBlockMapPtr->block[dieNo][blockNo].invalidSliceCnt);
}

static void test_invalidate_moves_block_between_victim_buckets(void)
{
	unsigned int vsa0 = AddrTransWrite(0);
	unsigned int dieNo = Vsa2VdieTranslation(vsa0);
	unsigned int blockNo = Vsa2VblockTranslation(vsa0);
	unsigned int lsa;

	/* land a second slice in the same die/block: USER_DIES writes later */
	for (lsa = 1; lsa < USER_DIES; lsa++)
		AddrTransWrite(lsa);
	AddrTransWrite(USER_DIES);
	TEST_ASSERT_EQUAL_UINT(dieNo, Vsa2VdieTranslation(AddrTransRead(USER_DIES)));

	InvalidateOldVsa(0);
	TEST_ASSERT_EQUAL_UINT(blockNo, gcVictimMapPtr->gcVictimList[dieNo][1].headBlock);

	InvalidateOldVsa(USER_DIES);
	TEST_ASSERT_EQUAL_UINT(BLOCK_NONE, gcVictimMapPtr->gcVictimList[dieNo][1].headBlock);
	TEST_ASSERT_EQUAL_UINT(blockNo, gcVictimMapPtr->gcVictimList[dieNo][2].headBlock);
	TEST_ASSERT_EQUAL_UINT(2, virtualBlockMapPtr->block[dieNo][blockNo].invalidSliceCnt);
}

/* ---------------------------------------------------------------- free block list */

static void test_init_leaves_every_die_with_current_block_and_free_list(void)
{
	unsigned int dieNo;

	for (dieNo = 0; dieNo < USER_DIES; dieNo++) {
		TEST_ASSERT_NOT_EQUAL(BLOCK_NONE, virtualDieMapPtr->die[dieNo].currentBlock);
		TEST_ASSERT_EQUAL_UINT(0, virtualBlockMapPtr->block[dieNo][virtualDieMapPtr->die[dieNo].currentBlock].free);
		/* all user blocks are good on the ideal NAND, minus one taken as current block */
		TEST_ASSERT_EQUAL_UINT(USER_BLOCKS_PER_DIE - 1, virtualDieMapPtr->die[dieNo].freeBlockCnt);
		TEST_ASSERT_NOT_EQUAL(BLOCK_NONE, virtualDieMapPtr->die[dieNo].headFreeBlock);
		TEST_ASSERT_EQUAL_UINT(BLOCK_NONE, virtualBlockMapPtr->block[dieNo][virtualDieMapPtr->die[dieNo].headFreeBlock].prevBlock);
		TEST_ASSERT_EQUAL_UINT(BLOCK_NONE, virtualBlockMapPtr->block[dieNo][virtualDieMapPtr->die[dieNo].tailFreeBlock].nextBlock);
	}
}

static void test_get_from_fb_list_pops_head_in_fifo_order(void)
{
	const unsigned int dieNo = 1;
	unsigned int head = virtualDieMapPtr->die[dieNo].headFreeBlock;
	unsigned int next = virtualBlockMapPtr->block[dieNo][head].nextBlock;
	unsigned int cnt = virtualDieMapPtr->die[dieNo].freeBlockCnt;
	unsigned int got = GetFromFbList(dieNo, GET_FREE_BLOCK_NORMAL);

	TEST_ASSERT_EQUAL_UINT(head, got);
	TEST_ASSERT_EQUAL_UINT(next, virtualDieMapPtr->die[dieNo].headFreeBlock);
	TEST_ASSERT_EQUAL_UINT(BLOCK_NONE, virtualBlockMapPtr->block[dieNo][next].prevBlock);
	TEST_ASSERT_EQUAL_UINT(BLOCK_NONE, virtualBlockMapPtr->block[dieNo][got].nextBlock);
	TEST_ASSERT_EQUAL_UINT(BLOCK_NONE, virtualBlockMapPtr->block[dieNo][got].prevBlock);
	TEST_ASSERT_EQUAL_UINT(0, virtualBlockMapPtr->block[dieNo][got].free);
	TEST_ASSERT_EQUAL_UINT(cnt - 1, virtualDieMapPtr->die[dieNo].freeBlockCnt);
}

static void test_put_to_fb_list_appends_at_tail(void)
{
	const unsigned int dieNo = 0;
	unsigned int got = GetFromFbList(dieNo, GET_FREE_BLOCK_NORMAL);
	unsigned int oldTail = virtualDieMapPtr->die[dieNo].tailFreeBlock;
	unsigned int cnt = virtualDieMapPtr->die[dieNo].freeBlockCnt;

	PutToFbList(dieNo, got);

	TEST_ASSERT_EQUAL_UINT(got, virtualDieMapPtr->die[dieNo].tailFreeBlock);
	TEST_ASSERT_EQUAL_UINT(got, virtualBlockMapPtr->block[dieNo][oldTail].nextBlock);
	TEST_ASSERT_EQUAL_UINT(oldTail, virtualBlockMapPtr->block[dieNo][got].prevBlock);
	TEST_ASSERT_EQUAL_UINT(BLOCK_NONE, virtualBlockMapPtr->block[dieNo][got].nextBlock);
	TEST_ASSERT_EQUAL_UINT(cnt + 1, virtualDieMapPtr->die[dieNo].freeBlockCnt);
}

static void test_normal_get_keeps_reserved_free_blocks(void)
{
	const unsigned int dieNo = 0;
	unsigned int head;

	while (virtualDieMapPtr->die[dieNo].freeBlockCnt > RESERVED_FREE_BLOCK_COUNT)
		TEST_ASSERT_NOT_EQUAL(BLOCK_FAIL, GetFromFbList(dieNo, GET_FREE_BLOCK_NORMAL));

	head = virtualDieMapPtr->die[dieNo].headFreeBlock;
	TEST_ASSERT_NOT_EQUAL(BLOCK_NONE, head);
	TEST_ASSERT_EQUAL_UINT(BLOCK_FAIL, GetFromFbList(dieNo, GET_FREE_BLOCK_NORMAL));
	TEST_ASSERT_EQUAL_UINT(head, virtualDieMapPtr->die[dieNo].headFreeBlock);
}

static void test_gc_get_drains_reserved_blocks_then_fails(void)
{
	const unsigned int dieNo = 0;
	unsigned int last;

	while (virtualDieMapPtr->die[dieNo].freeBlockCnt > RESERVED_FREE_BLOCK_COUNT)
		GetFromFbList(dieNo, GET_FREE_BLOCK_NORMAL);

	last = virtualDieMapPtr->die[dieNo].headFreeBlock;
	TEST_ASSERT_EQUAL_UINT(last, virtualDieMapPtr->die[dieNo].tailFreeBlock);

	TEST_ASSERT_EQUAL_UINT(last, GetFromFbList(dieNo, GET_FREE_BLOCK_GC));
	TEST_ASSERT_EQUAL_UINT(BLOCK_NONE, virtualDieMapPtr->die[dieNo].headFreeBlock);
	TEST_ASSERT_EQUAL_UINT(BLOCK_NONE, virtualDieMapPtr->die[dieNo].tailFreeBlock);
	TEST_ASSERT_EQUAL_UINT(0, virtualDieMapPtr->die[dieNo].freeBlockCnt);
	TEST_ASSERT_EQUAL_UINT(BLOCK_FAIL, GetFromFbList(dieNo, GET_FREE_BLOCK_GC));
}

static void test_put_to_empty_fb_list_sets_head_and_tail(void)
{
	const unsigned int dieNo = 0;
	unsigned int blockNo;

	while (virtualDieMapPtr->die[dieNo].headFreeBlock != BLOCK_NONE)
		GetFromFbList(dieNo, GET_FREE_BLOCK_GC);

	blockNo = virtualDieMapPtr->die[dieNo].currentBlock;
	PutToFbList(dieNo, blockNo);

	TEST_ASSERT_EQUAL_UINT(blockNo, virtualDieMapPtr->die[dieNo].headFreeBlock);
	TEST_ASSERT_EQUAL_UINT(blockNo, virtualDieMapPtr->die[dieNo].tailFreeBlock);
	TEST_ASSERT_EQUAL_UINT(BLOCK_NONE, virtualBlockMapPtr->block[dieNo][blockNo].prevBlock);
	TEST_ASSERT_EQUAL_UINT(BLOCK_NONE, virtualBlockMapPtr->block[dieNo][blockNo].nextBlock);
	TEST_ASSERT_EQUAL_UINT(1, virtualDieMapPtr->die[dieNo].freeBlockCnt);
}

static void test_get_from_fb_list_with_bad_option_asserts(void)
{
	FW_EXPECT_ASSERT(GetFromFbList(0, GET_FREE_BLOCK_GC + 1));
}

/* ---------------------------------------------------------------- erase */

static void test_erase_block_resets_metadata_and_issues_erase(void)
{
	const unsigned int dieNo = 2;
	unsigned int blockNo = GetFromFbList(dieNo, GET_FREE_BLOCK_NORMAL);
	unsigned int cnt = virtualDieMapPtr->die[dieNo].freeBlockCnt;
	unsigned int vsa0 = Vorg2VsaTranslation(dieNo, blockNo, 0);
	unsigned int vsaLast = Vorg2VsaTranslation(dieNo, blockNo, USER_PAGES_PER_BLOCK - 1);
	const mock_nsc_call_t *erase;

	/* currentPage must match the row-address dependency table (0 programs
	 * issued), otherwise the erase request blocks forever. */
	virtualBlockMapPtr->block[dieNo][blockNo].invalidSliceCnt = 5;
	virtualSliceMapPtr->virtualSlice[vsa0].logicalSliceAddr = 42;
	virtualSliceMapPtr->virtualSlice[vsaLast].logicalSliceAddr = 43;

	EraseBlock(dieNo, blockNo);
	SyncAllLowLevelReqDone();

	TEST_ASSERT_EQUAL_UINT(1, virtualBlockMapPtr->block[dieNo][blockNo].free);
	TEST_ASSERT_EQUAL_UINT(1, virtualBlockMapPtr->block[dieNo][blockNo].eraseCnt);
	TEST_ASSERT_EQUAL_UINT(0, virtualBlockMapPtr->block[dieNo][blockNo].invalidSliceCnt);
	TEST_ASSERT_EQUAL_UINT(0, virtualBlockMapPtr->block[dieNo][blockNo].currentPage);
	TEST_ASSERT_EQUAL_UINT(cnt + 1, virtualDieMapPtr->die[dieNo].freeBlockCnt);
	TEST_ASSERT_EQUAL_UINT(blockNo, virtualDieMapPtr->die[dieNo].tailFreeBlock);
	TEST_ASSERT_EQUAL_UINT(LSA_NONE, virtualSliceMapPtr->virtualSlice[vsa0].logicalSliceAddr);
	TEST_ASSERT_EQUAL_UINT(LSA_NONE, virtualSliceMapPtr->virtualSlice[vsaLast].logicalSliceAddr);

	TEST_ASSERT_EQUAL_UINT(1, mock_nsc_count_cmd(V2FCommand_BlockErase));
	erase = mock_nsc_call_at(0);
	TEST_ASSERT_NOT_NULL(erase);
	TEST_ASSERT_EQUAL_UINT(V2FCommand_BlockErase, erase->cmd);
	TEST_ASSERT_EQUAL_UINT(Vdie2PchTranslation(dieNo), erase->channel);
	TEST_ASSERT_EQUAL_INT(Vdie2PwayTranslation(dieNo), erase->way);
	TEST_ASSERT_EQUAL_HEX32(Vblock2PblockOfTbsTranslation(blockNo) * PAGES_PER_MLC_BLOCK, erase->rowAddress);
}

static void test_erase_block_in_lun1_targets_lun1_row(void)
{
	const unsigned int dieNo = 0;
	const unsigned int blockNo = USER_BLOCKS_PER_LUN + 2;
	const mock_nsc_call_t *erase;

	EraseBlock(dieNo, blockNo);
	SyncAllLowLevelReqDone();

	erase = mock_nsc_call_at(0);
	TEST_ASSERT_NOT_NULL(erase);
	TEST_ASSERT_EQUAL_HEX32(LUN_1_BASE_ADDR + 2 * PAGES_PER_MLC_BLOCK, erase->rowAddress);
}

static void test_erase_user_block_space_skips_bad_blocks(void)
{
	unsigned int expected = USER_DIES * USER_BLOCKS_PER_DIE - 2;

	virtualBlockMapPtr->block[0][0].bad = 1;
	virtualBlockMapPtr->block[USER_DIES - 1][USER_BLOCKS_PER_DIE - 1].bad = 1;

	EraseUserBlockSpace();

	TEST_ASSERT_EQUAL_UINT(expected, mock_nsc_count_cmd(V2FCommand_BlockErase));
}

static void test_erase_total_block_space_erases_every_physical_block(void)
{
	eraseSeen = 0;
	mock_nsc_set_hook(record_last_erase);

	EraseTotalBlockSpace();

	TEST_ASSERT_EQUAL_UINT(USER_DIES * TOTAL_BLOCKS_PER_DIE, mock_nsc_count_cmd(V2FCommand_BlockErase));
	TEST_ASSERT_TRUE(eraseSeen);
	TEST_ASSERT_EQUAL_HEX32(LUN_1_BASE_ADDR + (TOTAL_BLOCKS_PER_LUN - 1) * PAGES_PER_MLC_BLOCK, lastErase.rowAddress);
}

int main(void)
{
	UNITY_BEGIN();
	RUN_TEST(test_smoke_unwritten_lsa_translates_to_vsa_fail);
	RUN_TEST(test_smoke_write_then_read_maps_lsa_to_same_vsa);
	RUN_TEST(test_read_out_of_range_lsa_asserts);
	RUN_TEST(test_write_out_of_range_lsa_asserts);
	RUN_TEST(test_last_valid_lsa_is_accepted);
	RUN_TEST(test_translation_macros_round_trip_vsa);
	RUN_TEST(test_block_space_translation_macros);
	RUN_TEST(test_find_die_round_robins_channels_then_ways);
	RUN_TEST(test_consecutive_writes_spread_over_dies);
	RUN_TEST(test_write_bumps_current_page_of_target_die);
	RUN_TEST(test_rewrite_invalidates_old_vsa_and_queues_gc_victim);
	RUN_TEST(test_invalidate_unmapped_lsa_is_noop);
	RUN_TEST(test_invalidate_skips_vsa_owned_by_other_lsa);
	RUN_TEST(test_invalidate_moves_block_between_victim_buckets);
	RUN_TEST(test_init_leaves_every_die_with_current_block_and_free_list);
	RUN_TEST(test_get_from_fb_list_pops_head_in_fifo_order);
	RUN_TEST(test_put_to_fb_list_appends_at_tail);
	RUN_TEST(test_normal_get_keeps_reserved_free_blocks);
	RUN_TEST(test_gc_get_drains_reserved_blocks_then_fails);
	RUN_TEST(test_put_to_empty_fb_list_sets_head_and_tail);
	RUN_TEST(test_get_from_fb_list_with_bad_option_asserts);
	RUN_TEST(test_erase_block_resets_metadata_and_issues_erase);
	RUN_TEST(test_erase_block_in_lun1_targets_lun1_row);
	RUN_TEST(test_erase_user_block_space_skips_bad_blocks);
	RUN_TEST(test_erase_total_block_space_erases_every_physical_block);
	return UNITY_END();
}
