// Host unit tests for garbage_collection.c
#include <sys/mman.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include "minitest.h"
#include "memory_map.h"
#include "ftl_stubs.h"

#define DIE0	0
#define DIE1	1
#define LAST_DIE	(USER_DIES - 1)
#define BLK_A	10
#define BLK_B	20
#define BLK_C	30
#define LAST_BLK	(USER_BLOCKS_PER_DIE - 1)

static void setUp(void)
{
	stub_reset();
}

static void tearDown(void)
{
}

static GC_VICTIM_LIST_ENTRY *victimList(unsigned int die, unsigned int cnt)
{
	return &gcVictimMapPtr->gcVictimList[die][cnt];
}

static VIRTUAL_BLOCK_ENTRY *blk(unsigned int die, unsigned int block)
{
	return &virtualBlockMapPtr->block[die][block];
}

// Marks page `pageNo` of (die, block) as holding valid data for logical slice `lsa`.
static unsigned int mapValidPage(unsigned int die, unsigned int block, unsigned int pageNo, unsigned int lsa)
{
	unsigned int vsa = Vorg2VsaTranslation(die, block, pageNo);
	virtualSliceMapPtr->virtualSlice[vsa].logicalSliceAddr = lsa;
	logicalSliceMapPtr->logicalSlice[lsa].virtualSliceAddr = vsa;
	return vsa;
}

/* ---------------- InitGcVictimMap ---------------- */

static void test_InitGcVictimMap_sets_every_list_empty(void)
{
	// The firmware places the map at a fixed DRAM address; reserve that range on the host.
	uintptr_t mapAddr = (uintptr_t)GC_VICTIM_MAP_ADDR;
	uintptr_t pageMask = ~(uintptr_t)(sysconf(_SC_PAGESIZE) - 1);
	void *fixed = (void *)(mapAddr & pageMask);
	size_t len = (mapAddr - (uintptr_t)fixed) + sizeof(GC_VICTIM_MAP);
	void *p = mmap(fixed, len, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
	if(p != fixed)
		TEST_IGNORE_MESSAGE("could not map GC_VICTIM_MAP_ADDR on this host");
	memset(p, 0, len);

	InitGcVictimMap();

	TEST_ASSERT_TRUE(gcVictimMapPtr == (P_GC_VICTIM_MAP)mapAddr);
	for(unsigned int die = 0; die < USER_DIES; die++)
		for(unsigned int cnt = 0; cnt <= SLICES_PER_BLOCK; cnt++)
		{
			TEST_ASSERT_EQUAL_UINT(BLOCK_NONE, victimList(die, cnt)->headBlock);
			TEST_ASSERT_EQUAL_UINT(BLOCK_NONE, victimList(die, cnt)->tailBlock);
		}
	munmap(p, len);
}

/* ---------------- PutToGcVictimList ---------------- */

static void test_PutToGcVictimList_first_block_becomes_head_and_tail(void)
{
	PutToGcVictimList(DIE0, BLK_A, 5);

	TEST_ASSERT_EQUAL_UINT(BLK_A, victimList(DIE0, 5)->headBlock);
	TEST_ASSERT_EQUAL_UINT(BLK_A, victimList(DIE0, 5)->tailBlock);
	TEST_ASSERT_EQUAL_UINT(BLOCK_NONE, blk(DIE0, BLK_A)->prevBlock);
	TEST_ASSERT_EQUAL_UINT(BLOCK_NONE, blk(DIE0, BLK_A)->nextBlock);
}

static void test_PutToGcVictimList_second_block_is_appended_at_tail(void)
{
	PutToGcVictimList(DIE0, BLK_A, 5);
	PutToGcVictimList(DIE0, BLK_B, 5);

	TEST_ASSERT_EQUAL_UINT(BLK_A, victimList(DIE0, 5)->headBlock);
	TEST_ASSERT_EQUAL_UINT(BLK_B, victimList(DIE0, 5)->tailBlock);
	TEST_ASSERT_EQUAL_UINT(BLK_B, blk(DIE0, BLK_A)->nextBlock);
	TEST_ASSERT_EQUAL_UINT(BLK_A, blk(DIE0, BLK_B)->prevBlock);
	TEST_ASSERT_EQUAL_UINT(BLOCK_NONE, blk(DIE0, BLK_B)->nextBlock);
}

static void test_PutToGcVictimList_boundary_die_block_and_count(void)
{
	PutToGcVictimList(LAST_DIE, LAST_BLK, SLICES_PER_BLOCK);

	TEST_ASSERT_EQUAL_UINT(LAST_BLK, victimList(LAST_DIE, SLICES_PER_BLOCK)->headBlock);
	TEST_ASSERT_EQUAL_UINT(LAST_BLK, victimList(LAST_DIE, SLICES_PER_BLOCK)->tailBlock);
	TEST_ASSERT_EQUAL_UINT(BLOCK_NONE, victimList(DIE0, SLICES_PER_BLOCK)->headBlock);
}

/* ---------------- GetFromGcVictimList ---------------- */

static void test_GetFromGcVictimList_prefers_highest_invalid_count(void)
{
	PutToGcVictimList(DIE0, BLK_A, 3);
	PutToGcVictimList(DIE0, BLK_B, SLICES_PER_BLOCK);
	PutToGcVictimList(DIE0, BLK_C, 7);

	TEST_ASSERT_EQUAL_UINT(BLK_B, GetFromGcVictimList(DIE0));
	TEST_ASSERT_EQUAL_UINT(BLK_C, GetFromGcVictimList(DIE0));
	TEST_ASSERT_EQUAL_UINT(BLK_A, GetFromGcVictimList(DIE0));
}

static void test_GetFromGcVictimList_single_entry_empties_list(void)
{
	PutToGcVictimList(DIE0, BLK_A, 5);

	TEST_ASSERT_EQUAL_UINT(BLK_A, GetFromGcVictimList(DIE0));
	TEST_ASSERT_EQUAL_UINT(BLOCK_NONE, victimList(DIE0, 5)->headBlock);
	TEST_ASSERT_EQUAL_UINT(BLOCK_NONE, victimList(DIE0, 5)->tailBlock);
}

static void test_GetFromGcVictimList_advances_head_of_multi_entry_list(void)
{
	PutToGcVictimList(DIE0, BLK_A, 5);
	PutToGcVictimList(DIE0, BLK_B, 5);

	TEST_ASSERT_EQUAL_UINT(BLK_A, GetFromGcVictimList(DIE0));
	TEST_ASSERT_EQUAL_UINT(BLK_B, victimList(DIE0, 5)->headBlock);
	TEST_ASSERT_EQUAL_UINT(BLK_B, victimList(DIE0, 5)->tailBlock);
	TEST_ASSERT_EQUAL_UINT(BLOCK_NONE, blk(DIE0, BLK_B)->prevBlock);
}

static void test_GetFromGcVictimList_empty_die_returns_BLOCK_FAIL(void)
{
	TEST_ASSERT_EQUAL_UINT(BLOCK_FAIL, GetFromGcVictimList(DIE0));
	TEST_ASSERT_EQUAL_UINT(BLOCK_FAIL, GetFromGcVictimList(LAST_DIE));
}

static void test_GetFromGcVictimList_ignores_zero_invalid_count_list(void)
{
	// A block with no invalid slices is never a useful victim; the scan stops at count 1.
	PutToGcVictimList(DIE0, BLK_A, 0);

	TEST_ASSERT_EQUAL_UINT(BLOCK_FAIL, GetFromGcVictimList(DIE0));
	TEST_ASSERT_EQUAL_UINT(BLK_A, victimList(DIE0, 0)->headBlock);
}

static void test_GetFromGcVictimList_does_not_borrow_from_other_die(void)
{
	PutToGcVictimList(DIE1, BLK_A, 5);

	TEST_ASSERT_EQUAL_UINT(BLOCK_FAIL, GetFromGcVictimList(DIE0));
	TEST_ASSERT_EQUAL_UINT(BLK_A, GetFromGcVictimList(DIE1));
}

/* ---------------- GarbageCollection ---------------- */

static void test_GarbageCollection_empty_victim_list_does_nothing(void)
{
	GarbageCollection(DIE0);

	TEST_ASSERT_EQUAL_UINT(0, stub.eraseBlockCalls);
	TEST_ASSERT_EQUAL_UINT(0, stub.selectLowLevelReqQCalls);
	TEST_ASSERT_EQUAL_UINT(0, stub.allocateTempDataBufCalls);
	TEST_ASSERT_EQUAL_UINT(0, stub.findFreeVsaForGcCalls);
}

static void test_GarbageCollection_empty_list_leaves_block_map_untouched(void)
{
	VIRTUAL_BLOCK_MAP *snapshot = malloc(sizeof(VIRTUAL_BLOCK_MAP));
	memcpy(snapshot, virtualBlockMapPtr, sizeof(VIRTUAL_BLOCK_MAP));

	GarbageCollection(LAST_DIE);

	TEST_ASSERT_TRUE(memcmp(snapshot, virtualBlockMapPtr, sizeof(VIRTUAL_BLOCK_MAP)) == 0);
	free(snapshot);
}

static void test_GarbageCollection_fully_invalid_block_is_erased_without_copy(void)
{
	blk(DIE0, BLK_A)->invalidSliceCnt = SLICES_PER_BLOCK;
	PutToGcVictimList(DIE0, BLK_A, SLICES_PER_BLOCK);

	GarbageCollection(DIE0);

	TEST_ASSERT_EQUAL_UINT(1, stub.eraseBlockCalls);
	TEST_ASSERT_EQUAL_UINT(DIE0, stub.lastEraseDie);
	TEST_ASSERT_EQUAL_UINT(BLK_A, stub.lastEraseBlock);
	TEST_ASSERT_EQUAL_UINT(0, stub.selectLowLevelReqQCalls);
	TEST_ASSERT_EQUAL_UINT(BLOCK_NONE, victimList(DIE0, SLICES_PER_BLOCK)->headBlock);
}

static void test_GarbageCollection_copies_valid_pages_then_erases(void)
{
	const unsigned int lsa0 = 1000, lsa1 = 2000;
	blk(DIE1, BLK_B)->invalidSliceCnt = SLICES_PER_BLOCK - 2;
	PutToGcVictimList(DIE1, BLK_B, SLICES_PER_BLOCK - 2);
	mapValidPage(DIE1, BLK_B, 0, lsa0);
	mapValidPage(DIE1, BLK_B, USER_PAGES_PER_BLOCK - 1, lsa1);

	GarbageCollection(DIE1);

	// one read + one write request per valid page
	TEST_ASSERT_EQUAL_UINT(4, stub.selectLowLevelReqQCalls);
	TEST_ASSERT_EQUAL_UINT(4, stub.allocateTempDataBufCalls);
	TEST_ASSERT_EQUAL_UINT(4, stub.updateTempDataBufCalls);
	TEST_ASSERT_EQUAL_UINT(2, stub.findFreeVsaForGcCalls);
	TEST_ASSERT_EQUAL_UINT(DIE1, stub.lastGcCopyDie);
	TEST_ASSERT_EQUAL_UINT(BLK_B, stub.lastGcVictimBlock);

	// the mappings now point at the freshly allocated slices
	const unsigned int copy0 = STUB_GC_COPY_VSA(DIE1, 0), copy1 = STUB_GC_COPY_VSA(DIE1, 1);
	TEST_ASSERT_EQUAL_UINT(DIE1, Vsa2VdieTranslation(copy0));
	TEST_ASSERT_EQUAL_UINT(copy0, logicalSliceMapPtr->logicalSlice[lsa0].virtualSliceAddr);
	TEST_ASSERT_EQUAL_UINT(copy1, logicalSliceMapPtr->logicalSlice[lsa1].virtualSliceAddr);
	TEST_ASSERT_EQUAL_UINT(lsa0, virtualSliceMapPtr->virtualSlice[copy0].logicalSliceAddr);
	TEST_ASSERT_EQUAL_UINT(lsa1, virtualSliceMapPtr->virtualSlice[copy1].logicalSliceAddr);

	TEST_ASSERT_EQUAL_UINT(1, stub.eraseBlockCalls);
	TEST_ASSERT_EQUAL_UINT(DIE1, stub.lastEraseDie);
	TEST_ASSERT_EQUAL_UINT(BLK_B, stub.lastEraseBlock);
}

static void test_GarbageCollection_read_and_write_requests_are_well_formed(void)
{
	const unsigned int lsa = 4242;
	blk(DIE0, BLK_A)->invalidSliceCnt = 1;
	PutToGcVictimList(DIE0, BLK_A, 1);
	unsigned int vsa = mapValidPage(DIE0, BLK_A, 3, lsa);

	GarbageCollection(DIE0);

	TEST_ASSERT_EQUAL_UINT(2, stub.selectLowLevelReqQCalls);
	SSD_REQ_FORMAT *rd = &reqPoolPtr->reqPool[stub.issuedReqSlotTags[0]];
	SSD_REQ_FORMAT *wr = &reqPoolPtr->reqPool[stub.issuedReqSlotTags[1]];

	TEST_ASSERT_EQUAL_UINT(REQ_TYPE_NAND, rd->reqType);
	TEST_ASSERT_EQUAL_UINT(REQ_CODE_READ, rd->reqCode);
	TEST_ASSERT_EQUAL_UINT(lsa, rd->logicalSliceAddr);
	TEST_ASSERT_EQUAL_UINT(vsa, rd->nandInfo.virtualSliceAddr);
	TEST_ASSERT_EQUAL_UINT(REQ_OPT_DATA_BUF_TEMP_ENTRY, rd->reqOpt.dataBufFormat);
	TEST_ASSERT_EQUAL_UINT(REQ_OPT_NAND_ADDR_VSA, rd->reqOpt.nandAddr);
	TEST_ASSERT_EQUAL_UINT(REQ_OPT_NAND_ECC_ON, rd->reqOpt.nandEcc);
	TEST_ASSERT_EQUAL_UINT(REQ_OPT_ROW_ADDR_DEPENDENCY_CHECK, rd->reqOpt.rowAddrDependencyCheck);
	TEST_ASSERT_EQUAL_UINT(REQ_OPT_BLOCK_SPACE_MAIN, rd->reqOpt.blockSpace);

	TEST_ASSERT_EQUAL_UINT(REQ_TYPE_NAND, wr->reqType);
	TEST_ASSERT_EQUAL_UINT(REQ_CODE_WRITE, wr->reqCode);
	TEST_ASSERT_EQUAL_UINT(lsa, wr->logicalSliceAddr);
	TEST_ASSERT_EQUAL_UINT(STUB_GC_COPY_VSA(DIE0, 0), wr->nandInfo.virtualSliceAddr);
	TEST_ASSERT_EQUAL_UINT(REQ_OPT_DATA_BUF_TEMP_ENTRY, wr->reqOpt.dataBufFormat);
}

static void test_GarbageCollection_skips_stale_pages(void)
{
	// The page still names a logical slice, but that slice has since been rewritten elsewhere.
	const unsigned int lsa = 77;
	blk(DIE0, BLK_A)->invalidSliceCnt = 1;
	PutToGcVictimList(DIE0, BLK_A, 1);
	unsigned int vsa = Vorg2VsaTranslation(DIE0, BLK_A, 2);
	virtualSliceMapPtr->virtualSlice[vsa].logicalSliceAddr = lsa;
	logicalSliceMapPtr->logicalSlice[lsa].virtualSliceAddr = vsa + USER_DIES;

	GarbageCollection(DIE0);

	TEST_ASSERT_EQUAL_UINT(0, stub.selectLowLevelReqQCalls);
	TEST_ASSERT_EQUAL_UINT(vsa + USER_DIES, logicalSliceMapPtr->logicalSlice[lsa].virtualSliceAddr);
	TEST_ASSERT_EQUAL_UINT(1, stub.eraseBlockCalls);
}

static void test_GarbageCollection_second_call_on_drained_die_is_noop(void)
{
	blk(DIE0, BLK_A)->invalidSliceCnt = SLICES_PER_BLOCK;
	PutToGcVictimList(DIE0, BLK_A, SLICES_PER_BLOCK);

	GarbageCollection(DIE0);
	GarbageCollection(DIE0);

	TEST_ASSERT_EQUAL_UINT(1, stub.eraseBlockCalls);
}

/* ---------------- SelectiveGetFromGcVictimList ---------------- */

static void test_SelectiveGet_removes_middle_block(void)
{
	blk(DIE0, BLK_A)->invalidSliceCnt = blk(DIE0, BLK_B)->invalidSliceCnt = blk(DIE0, BLK_C)->invalidSliceCnt = 5;
	PutToGcVictimList(DIE0, BLK_A, 5);
	PutToGcVictimList(DIE0, BLK_B, 5);
	PutToGcVictimList(DIE0, BLK_C, 5);

	SelectiveGetFromGcVictimList(DIE0, BLK_B);

	TEST_ASSERT_EQUAL_UINT(BLK_C, blk(DIE0, BLK_A)->nextBlock);
	TEST_ASSERT_EQUAL_UINT(BLK_A, blk(DIE0, BLK_C)->prevBlock);
	TEST_ASSERT_EQUAL_UINT(BLK_A, victimList(DIE0, 5)->headBlock);
	TEST_ASSERT_EQUAL_UINT(BLK_C, victimList(DIE0, 5)->tailBlock);
}

static void test_SelectiveGet_removes_tail_block(void)
{
	blk(DIE0, BLK_A)->invalidSliceCnt = blk(DIE0, BLK_B)->invalidSliceCnt = 5;
	PutToGcVictimList(DIE0, BLK_A, 5);
	PutToGcVictimList(DIE0, BLK_B, 5);

	SelectiveGetFromGcVictimList(DIE0, BLK_B);

	TEST_ASSERT_EQUAL_UINT(BLOCK_NONE, blk(DIE0, BLK_A)->nextBlock);
	TEST_ASSERT_EQUAL_UINT(BLK_A, victimList(DIE0, 5)->tailBlock);
	TEST_ASSERT_EQUAL_UINT(BLK_A, victimList(DIE0, 5)->headBlock);
}

static void test_SelectiveGet_removes_head_block(void)
{
	blk(DIE0, BLK_A)->invalidSliceCnt = blk(DIE0, BLK_B)->invalidSliceCnt = 5;
	PutToGcVictimList(DIE0, BLK_A, 5);
	PutToGcVictimList(DIE0, BLK_B, 5);

	SelectiveGetFromGcVictimList(DIE0, BLK_A);

	TEST_ASSERT_EQUAL_UINT(BLOCK_NONE, blk(DIE0, BLK_B)->prevBlock);
	TEST_ASSERT_EQUAL_UINT(BLK_B, victimList(DIE0, 5)->headBlock);
	TEST_ASSERT_EQUAL_UINT(BLK_B, victimList(DIE0, 5)->tailBlock);
}

static void test_SelectiveGet_removes_only_block(void)
{
	blk(DIE0, BLK_A)->invalidSliceCnt = 5;
	PutToGcVictimList(DIE0, BLK_A, 5);

	SelectiveGetFromGcVictimList(DIE0, BLK_A);

	TEST_ASSERT_EQUAL_UINT(BLOCK_NONE, victimList(DIE0, 5)->headBlock);
	TEST_ASSERT_EQUAL_UINT(BLOCK_NONE, victimList(DIE0, 5)->tailBlock);
	TEST_ASSERT_EQUAL_UINT(BLOCK_FAIL, GetFromGcVictimList(DIE0));
}

int main(void)
{
	UNITY_BEGIN();
	RUN_TEST(test_InitGcVictimMap_sets_every_list_empty);
	RUN_TEST(test_PutToGcVictimList_first_block_becomes_head_and_tail);
	RUN_TEST(test_PutToGcVictimList_second_block_is_appended_at_tail);
	RUN_TEST(test_PutToGcVictimList_boundary_die_block_and_count);
	RUN_TEST(test_GetFromGcVictimList_prefers_highest_invalid_count);
	RUN_TEST(test_GetFromGcVictimList_single_entry_empties_list);
	RUN_TEST(test_GetFromGcVictimList_advances_head_of_multi_entry_list);
	RUN_TEST(test_GetFromGcVictimList_empty_die_returns_BLOCK_FAIL);
	RUN_TEST(test_GetFromGcVictimList_ignores_zero_invalid_count_list);
	RUN_TEST(test_GetFromGcVictimList_does_not_borrow_from_other_die);
	RUN_TEST(test_GarbageCollection_empty_victim_list_does_nothing);
	RUN_TEST(test_GarbageCollection_empty_list_leaves_block_map_untouched);
	RUN_TEST(test_GarbageCollection_fully_invalid_block_is_erased_without_copy);
	RUN_TEST(test_GarbageCollection_copies_valid_pages_then_erases);
	RUN_TEST(test_GarbageCollection_read_and_write_requests_are_well_formed);
	RUN_TEST(test_GarbageCollection_skips_stale_pages);
	RUN_TEST(test_GarbageCollection_second_call_on_drained_die_is_noop);
	RUN_TEST(test_SelectiveGet_removes_middle_block);
	RUN_TEST(test_SelectiveGet_removes_tail_block);
	RUN_TEST(test_SelectiveGet_removes_head_block);
	RUN_TEST(test_SelectiveGet_removes_only_block);
	return UNITY_END();
}
