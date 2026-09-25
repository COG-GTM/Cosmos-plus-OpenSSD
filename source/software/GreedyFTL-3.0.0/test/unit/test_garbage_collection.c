/* Unit tests for garbage_collection.c. */
#include "unity.h"

#include "fw_test.h"
#include "memory_map.h"

#define TEST_DIE 0

void setUp(void)
{
	fw_test_reset();
	fw_test_init_ftl();
	mock_nsc_reset();
}

void tearDown(void) {}

static GC_VICTIM_LIST_ENTRY *victims(unsigned int invalidSliceCnt)
{
	return &gcVictimMapPtr->gcVictimList[TEST_DIE][invalidSliceCnt];
}

static VIRTUAL_BLOCK_ENTRY *block(unsigned int blockNo)
{
	return &virtualBlockMapPtr->block[TEST_DIE][blockNo];
}

/* Writes one slice per die so that TEST_DIE receives exactly one page. */
static unsigned int write_one_page_on_every_die(unsigned int firstLsa)
{
	unsigned int lsa, vsaOnTestDie = VSA_NONE;

	for (lsa = firstLsa; lsa < firstLsa + USER_DIES; lsa++) {
		unsigned int vsa = AddrTransWrite(lsa);

		if (Vsa2VdieTranslation(vsa) == TEST_DIE)
			vsaOnTestDie = vsa;
	}
	TEST_ASSERT_NOT_EQUAL(VSA_NONE, vsaOnTestDie);
	return vsaOnTestDie;
}

static void permit_block_pages(unsigned int blockNo, unsigned int pages)
{
	unsigned int chNo = Vdie2PchTranslation(TEST_DIE);
	unsigned int wayNo = Vdie2PwayTranslation(TEST_DIE);

	rowAddrDependencyTablePtr->block[chNo][wayNo][blockNo].permittedProgPage = pages;
}

static void test_smoke_victim_list_returns_queued_block(void)
{
	const unsigned int blockNo = 7;

	PutToGcVictimList(TEST_DIE, blockNo, 3);

	TEST_ASSERT_EQUAL_UINT(blockNo, GetFromGcVictimList(TEST_DIE));
}

static void test_smoke_empty_victim_list_asserts(void)
{
	FW_EXPECT_ASSERT(GetFromGcVictimList(TEST_DIE));
}

static void test_init_clears_every_victim_list(void)
{
	unsigned int dieNo, cnt;

	for (dieNo = 0; dieNo < USER_DIES; dieNo++)
		for (cnt = 0; cnt <= SLICES_PER_BLOCK; cnt++) {
			TEST_ASSERT_EQUAL_UINT(BLOCK_NONE, gcVictimMapPtr->gcVictimList[dieNo][cnt].headBlock);
			TEST_ASSERT_EQUAL_UINT(BLOCK_NONE, gcVictimMapPtr->gcVictimList[dieNo][cnt].tailBlock);
		}
}

static void test_victim_list_keeps_fifo_order_within_same_count(void)
{
	PutToGcVictimList(TEST_DIE, 10, 2);
	PutToGcVictimList(TEST_DIE, 11, 2);
	PutToGcVictimList(TEST_DIE, 12, 2);

	TEST_ASSERT_EQUAL_UINT(10, victims(2)->headBlock);
	TEST_ASSERT_EQUAL_UINT(12, victims(2)->tailBlock);
	TEST_ASSERT_EQUAL_UINT(11, block(10)->nextBlock);
	TEST_ASSERT_EQUAL_UINT(10, block(11)->prevBlock);

	TEST_ASSERT_EQUAL_UINT(10, GetFromGcVictimList(TEST_DIE));
	TEST_ASSERT_EQUAL_UINT(11, victims(2)->headBlock);
	TEST_ASSERT_EQUAL_UINT(BLOCK_NONE, block(11)->prevBlock);
	TEST_ASSERT_EQUAL_UINT(11, GetFromGcVictimList(TEST_DIE));
	TEST_ASSERT_EQUAL_UINT(12, GetFromGcVictimList(TEST_DIE));
	TEST_ASSERT_EQUAL_UINT(BLOCK_NONE, victims(2)->headBlock);
	TEST_ASSERT_EQUAL_UINT(BLOCK_NONE, victims(2)->tailBlock);
}

static void test_victim_with_most_invalid_slices_is_picked_first(void)
{
	PutToGcVictimList(TEST_DIE, 20, 1);
	PutToGcVictimList(TEST_DIE, 21, SLICES_PER_BLOCK);
	PutToGcVictimList(TEST_DIE, 22, 5);

	TEST_ASSERT_EQUAL_UINT(21, GetFromGcVictimList(TEST_DIE));
	TEST_ASSERT_EQUAL_UINT(22, GetFromGcVictimList(TEST_DIE));
	TEST_ASSERT_EQUAL_UINT(20, GetFromGcVictimList(TEST_DIE));
}

static void test_blocks_with_zero_invalid_slices_are_never_victims(void)
{
	PutToGcVictimList(TEST_DIE, 30, 0);

	FW_EXPECT_ASSERT(GetFromGcVictimList(TEST_DIE));
}

static void test_victim_lists_are_per_die(void)
{
	PutToGcVictimList(USER_DIES - 1, 40, 3);

	FW_EXPECT_ASSERT(GetFromGcVictimList(TEST_DIE));
	TEST_ASSERT_EQUAL_UINT(40, GetFromGcVictimList(USER_DIES - 1));
}

static void test_selective_removal_handles_middle_head_tail_and_only(void)
{
	block(50)->invalidSliceCnt = 4;
	block(51)->invalidSliceCnt = 4;
	block(52)->invalidSliceCnt = 4;
	PutToGcVictimList(TEST_DIE, 50, 4);
	PutToGcVictimList(TEST_DIE, 51, 4);
	PutToGcVictimList(TEST_DIE, 52, 4);

	SelectiveGetFromGcVictimList(TEST_DIE, 51);
	TEST_ASSERT_EQUAL_UINT(52, block(50)->nextBlock);
	TEST_ASSERT_EQUAL_UINT(50, block(52)->prevBlock);

	SelectiveGetFromGcVictimList(TEST_DIE, 50);
	TEST_ASSERT_EQUAL_UINT(52, victims(4)->headBlock);
	TEST_ASSERT_EQUAL_UINT(BLOCK_NONE, block(52)->prevBlock);

	PutToGcVictimList(TEST_DIE, 51, 4);
	SelectiveGetFromGcVictimList(TEST_DIE, 51);
	TEST_ASSERT_EQUAL_UINT(52, victims(4)->tailBlock);
	TEST_ASSERT_EQUAL_UINT(BLOCK_NONE, block(52)->nextBlock);

	SelectiveGetFromGcVictimList(TEST_DIE, 52);
	TEST_ASSERT_EQUAL_UINT(BLOCK_NONE, victims(4)->headBlock);
	TEST_ASSERT_EQUAL_UINT(BLOCK_NONE, victims(4)->tailBlock);
}

static void test_overwriting_a_slice_queues_its_block_as_victim(void)
{
	unsigned int vsa = write_one_page_on_every_die(0);
	unsigned int blockNo = Vsa2VblockTranslation(vsa);
	unsigned int lsa = virtualSliceMapPtr->virtualSlice[vsa].logicalSliceAddr;

	TEST_ASSERT_EQUAL_UINT(0, block(blockNo)->invalidSliceCnt);
	FW_EXPECT_ASSERT(GetFromGcVictimList(TEST_DIE));

	AddrTransWrite(lsa);

	TEST_ASSERT_EQUAL_UINT(1, block(blockNo)->invalidSliceCnt);
	TEST_ASSERT_EQUAL_UINT(blockNo, victims(1)->headBlock);
	TEST_ASSERT_EQUAL_UINT(blockNo, GetFromGcVictimList(TEST_DIE));
}

static void test_gc_copies_valid_slices_then_erases_victim(void)
{
	unsigned int staleVsa = write_one_page_on_every_die(0);
	unsigned int liveVsa = write_one_page_on_every_die(USER_DIES);
	unsigned int blockNo = Vsa2VblockTranslation(staleVsa);
	unsigned int staleLsa = virtualSliceMapPtr->virtualSlice[staleVsa].logicalSliceAddr;
	unsigned int liveLsa = virtualSliceMapPtr->virtualSlice[liveVsa].logicalSliceAddr;
	unsigned int newVsa, validSlices;

	TEST_ASSERT_EQUAL_UINT(blockNo, Vsa2VblockTranslation(liveVsa));
	TEST_ASSERT_EQUAL_UINT(2, block(blockNo)->currentPage);

	AddrTransWrite(staleLsa); /* invalidates staleVsa, block becomes a victim */
	TEST_ASSERT_EQUAL_UINT(1, block(blockNo)->invalidSliceCnt);
	/* The replacement slice may itself land in this block; count what GC must copy. */
	validSlices = block(blockNo)->currentPage - block(blockNo)->invalidSliceCnt;
	permit_block_pages(blockNo, block(blockNo)->currentPage);

	GarbageCollection(TEST_DIE);

	newVsa = logicalSliceMapPtr->logicalSlice[liveLsa].virtualSliceAddr;
	TEST_ASSERT_NOT_EQUAL(liveVsa, newVsa);
	TEST_ASSERT_NOT_EQUAL(blockNo, Vsa2VblockTranslation(newVsa));
	TEST_ASSERT_EQUAL_UINT(TEST_DIE, Vsa2VdieTranslation(newVsa));
	TEST_ASSERT_EQUAL_UINT(liveLsa, virtualSliceMapPtr->virtualSlice[newVsa].logicalSliceAddr);

	TEST_ASSERT_EQUAL_UINT(1, block(blockNo)->free);
	TEST_ASSERT_EQUAL_UINT(0, block(blockNo)->invalidSliceCnt);
	TEST_ASSERT_EQUAL_UINT(0, block(blockNo)->currentPage);
	TEST_ASSERT_EQUAL_UINT(1, block(blockNo)->eraseCnt);
	TEST_ASSERT_NOT_EQUAL(blockNo, virtualDieMapPtr->die[TEST_DIE].currentBlock);
	FW_EXPECT_ASSERT(GetFromGcVictimList(TEST_DIE));

	SyncAllLowLevelReqDone();
	TEST_ASSERT_EQUAL_UINT(validSlices, mock_nsc_count_cmd(V2FCommand_ReadPageTrigger));
	TEST_ASSERT_EQUAL_UINT(validSlices, mock_nsc_count_cmd(V2FCommand_ProgramPage));
	TEST_ASSERT_EQUAL_UINT(1, mock_nsc_count_cmd(V2FCommand_BlockErase));
}

static void test_gc_of_fully_invalid_block_only_erases(void)
{
	unsigned int vsa = write_one_page_on_every_die(0);
	unsigned int blockNo = Vsa2VblockTranslation(vsa);
	unsigned int lsa = virtualSliceMapPtr->virtualSlice[vsa].logicalSliceAddr;

	/* Drop the only live mapping so the block genuinely holds no valid data. */
	logicalSliceMapPtr->logicalSlice[lsa].virtualSliceAddr = VSA_NONE;
	block(blockNo)->invalidSliceCnt = SLICES_PER_BLOCK;
	PutToGcVictimList(TEST_DIE, blockNo, SLICES_PER_BLOCK);
	permit_block_pages(blockNo, block(blockNo)->currentPage);

	GarbageCollection(TEST_DIE);
	SyncAllLowLevelReqDone();

	TEST_ASSERT_EQUAL_UINT(0, mock_nsc_count_cmd(V2FCommand_ReadPageTrigger));
	TEST_ASSERT_EQUAL_UINT(0, mock_nsc_count_cmd(V2FCommand_ProgramPage));
	TEST_ASSERT_EQUAL_UINT(1, mock_nsc_count_cmd(V2FCommand_BlockErase));
	TEST_ASSERT_EQUAL_UINT(1, block(blockNo)->free);
	TEST_ASSERT_EQUAL_UINT(0, block(blockNo)->invalidSliceCnt);
	TEST_ASSERT_EQUAL_HEX32(VSA_NONE, logicalSliceMapPtr->logicalSlice[lsa].virtualSliceAddr);
}

int main(void)
{
	UNITY_BEGIN();
	RUN_TEST(test_smoke_victim_list_returns_queued_block);
	RUN_TEST(test_smoke_empty_victim_list_asserts);
	RUN_TEST(test_init_clears_every_victim_list);
	RUN_TEST(test_victim_list_keeps_fifo_order_within_same_count);
	RUN_TEST(test_victim_with_most_invalid_slices_is_picked_first);
	RUN_TEST(test_blocks_with_zero_invalid_slices_are_never_victims);
	RUN_TEST(test_victim_lists_are_per_die);
	RUN_TEST(test_selective_removal_handles_middle_head_tail_and_only);
	RUN_TEST(test_overwriting_a_slice_queues_its_block_as_victim);
	RUN_TEST(test_gc_copies_valid_slices_then_erases_victim);
	RUN_TEST(test_gc_of_fully_invalid_block_only_erases);
	return UNITY_END();
}
