/* Unit tests for free virtual slice allocation in address_translation.c
 * (FindFreeVirtualSlice / FindFreeVirtualSliceForGc). GarbageCollection() is
 * wrapped (test_address_translation_alloc.wrap) so the block-exhaustion
 * paths can be driven deterministically. */
#include "unity.h"

#include "fw_test.h"
#include "memory_map.h"

static unsigned int gcCalls;
static unsigned int gcDie;
static unsigned int gcNextCurrentPage;
static unsigned int gcRefillFreeBlock;

void __real_GarbageCollection(unsigned int dieNo);

void __wrap_GarbageCollection(unsigned int dieNo)
{
	unsigned int blockNo = virtualDieMapPtr->die[dieNo].currentBlock;

	gcCalls++;
	gcDie = dieNo;
	virtualBlockMapPtr->block[dieNo][blockNo].currentPage = gcNextCurrentPage;
	if (gcRefillFreeBlock != BLOCK_NONE)
		PutToFbList(dieNo, gcRefillFreeBlock);
}

void setUp(void)
{
	fw_test_reset();
	fw_test_init_ftl();
	gcCalls = 0;
	gcDie = BLOCK_NONE;
	gcNextCurrentPage = 0;
	gcRefillFreeBlock = BLOCK_NONE;
}

void tearDown(void) {}

static unsigned int fill_current_block(unsigned int dieNo)
{
	unsigned int blockNo = virtualDieMapPtr->die[dieNo].currentBlock;

	virtualBlockMapPtr->block[dieNo][blockNo].currentPage = USER_PAGES_PER_BLOCK;
	return blockNo;
}

static void drain_free_blocks(unsigned int dieNo)
{
	while (virtualDieMapPtr->die[dieNo].freeBlockCnt > RESERVED_FREE_BLOCK_COUNT)
		GetFromFbList(dieNo, GET_FREE_BLOCK_NORMAL);
}

static void test_full_block_moves_to_next_free_block(void)
{
	unsigned int dieNo = sliceAllocationTargetDie;
	unsigned int fullBlock = fill_current_block(dieNo);
	unsigned int expected = virtualDieMapPtr->die[dieNo].headFreeBlock;
	unsigned int vsa = FindFreeVirtualSlice();

	TEST_ASSERT_EQUAL_UINT(dieNo, Vsa2VdieTranslation(vsa));
	TEST_ASSERT_EQUAL_UINT(expected, Vsa2VblockTranslation(vsa));
	TEST_ASSERT_EQUAL_UINT(0, Vsa2VpageTranslation(vsa));
	TEST_ASSERT_NOT_EQUAL(fullBlock, expected);
	TEST_ASSERT_EQUAL_UINT(expected, virtualDieMapPtr->die[dieNo].currentBlock);
	TEST_ASSERT_EQUAL_UINT(1, virtualBlockMapPtr->block[dieNo][expected].currentPage);
	TEST_ASSERT_EQUAL_UINT(0, gcCalls);
}

static void test_allocation_advances_target_die(void)
{
	unsigned int dieNo = sliceAllocationTargetDie;

	FindFreeVirtualSlice();

	TEST_ASSERT_EQUAL_UINT((dieNo + 1) % USER_DIES, sliceAllocationTargetDie);
}

static void test_no_free_block_triggers_gc_and_reuses_freed_current_block(void)
{
	unsigned int dieNo = sliceAllocationTargetDie;
	unsigned int fullBlock = fill_current_block(dieNo);
	unsigned int vsa;

	drain_free_blocks(dieNo);
	gcNextCurrentPage = 3;

	vsa = FindFreeVirtualSlice();

	TEST_ASSERT_EQUAL_UINT(1, gcCalls);
	TEST_ASSERT_EQUAL_UINT(dieNo, gcDie);
	TEST_ASSERT_EQUAL_UINT(fullBlock, Vsa2VblockTranslation(vsa));
	TEST_ASSERT_EQUAL_UINT(3, Vsa2VpageTranslation(vsa));
	TEST_ASSERT_EQUAL_UINT(4, virtualBlockMapPtr->block[dieNo][fullBlock].currentPage);
}

static void test_gc_that_refills_free_list_switches_current_block(void)
{
	unsigned int dieNo = sliceAllocationTargetDie;
	unsigned int fullBlock = fill_current_block(dieNo);
	unsigned int refill;
	unsigned int vsa;

	drain_free_blocks(dieNo);
	refill = GetFromFbList(dieNo, GET_FREE_BLOCK_GC);
	TEST_ASSERT_EQUAL_UINT(0, virtualDieMapPtr->die[dieNo].freeBlockCnt);

	gcNextCurrentPage = USER_PAGES_PER_BLOCK; /* GC did not free the current block */
	gcRefillFreeBlock = refill;
	virtualBlockMapPtr->block[dieNo][refill].currentPage = 0;
	/* RESERVED_FREE_BLOCK_COUNT blocks must stay behind, so GC returns two */
	PutToFbList(dieNo, fullBlock == 0 ? 1 : 0);

	vsa = FindFreeVirtualSlice();

	TEST_ASSERT_EQUAL_UINT(1, gcCalls);
	TEST_ASSERT_EQUAL_UINT(fullBlock == 0 ? 1 : 0, Vsa2VblockTranslation(vsa));
	TEST_ASSERT_EQUAL_UINT(fullBlock == 0 ? 1 : 0, virtualDieMapPtr->die[dieNo].currentBlock);
}

static void test_gc_that_frees_nothing_asserts(void)
{
	unsigned int dieNo = sliceAllocationTargetDie;

	fill_current_block(dieNo);
	drain_free_blocks(dieNo);
	gcNextCurrentPage = USER_PAGES_PER_BLOCK;

	FW_EXPECT_ASSERT(FindFreeVirtualSlice());
	TEST_ASSERT_EQUAL_UINT(1, gcCalls);
}

static void test_gc_leaving_overflowed_page_counter_asserts(void)
{
	unsigned int dieNo = sliceAllocationTargetDie;

	fill_current_block(dieNo);
	drain_free_blocks(dieNo);
	gcNextCurrentPage = USER_PAGES_PER_BLOCK + 1;

	FW_EXPECT_ASSERT(FindFreeVirtualSlice());
}

static void test_overflowed_current_page_asserts(void)
{
	unsigned int dieNo = sliceAllocationTargetDie;
	unsigned int blockNo = virtualDieMapPtr->die[dieNo].currentBlock;

	virtualBlockMapPtr->block[dieNo][blockNo].currentPage = USER_PAGES_PER_BLOCK + 1;

	FW_EXPECT_ASSERT(FindFreeVirtualSlice());
	TEST_ASSERT_EQUAL_UINT(0, gcCalls);
}

/* ---------------------------------------------------------------- GC allocation */

static void test_gc_alloc_uses_current_block_of_target_die(void)
{
	const unsigned int dieNo = 1;
	unsigned int blockNo = virtualDieMapPtr->die[dieNo].currentBlock;
	unsigned int target = sliceAllocationTargetDie;
	unsigned int vsa = FindFreeVirtualSliceForGc(dieNo, blockNo + 1);

	TEST_ASSERT_EQUAL_UINT(dieNo, Vsa2VdieTranslation(vsa));
	TEST_ASSERT_EQUAL_UINT(blockNo, Vsa2VblockTranslation(vsa));
	TEST_ASSERT_EQUAL_UINT(0, Vsa2VpageTranslation(vsa));
	TEST_ASSERT_EQUAL_UINT(1, virtualBlockMapPtr->block[dieNo][blockNo].currentPage);
	/* GC allocation must not disturb the host allocation cursor */
	TEST_ASSERT_EQUAL_UINT(target, sliceAllocationTargetDie);
}

static void test_gc_alloc_replaces_current_block_when_it_is_the_victim(void)
{
	const unsigned int dieNo = 0;
	unsigned int victim = virtualDieMapPtr->die[dieNo].currentBlock;
	unsigned int expected = virtualDieMapPtr->die[dieNo].headFreeBlock;
	unsigned int vsa = FindFreeVirtualSliceForGc(dieNo, victim);

	TEST_ASSERT_EQUAL_UINT(expected, virtualDieMapPtr->die[dieNo].currentBlock);
	TEST_ASSERT_EQUAL_UINT(expected, Vsa2VblockTranslation(vsa));
}

static void test_gc_alloc_with_victim_current_block_and_empty_list_asserts(void)
{
	const unsigned int dieNo = 0;
	unsigned int victim = virtualDieMapPtr->die[dieNo].currentBlock;

	while (virtualDieMapPtr->die[dieNo].headFreeBlock != BLOCK_NONE)
		GetFromFbList(dieNo, GET_FREE_BLOCK_GC);

	FW_EXPECT_ASSERT(FindFreeVirtualSliceForGc(dieNo, victim));
}

static void test_gc_alloc_on_full_block_takes_reserved_free_block(void)
{
	const unsigned int dieNo = 0;
	unsigned int fullBlock = fill_current_block(dieNo);
	unsigned int expected;
	unsigned int vsa;

	drain_free_blocks(dieNo);
	expected = virtualDieMapPtr->die[dieNo].headFreeBlock;
	TEST_ASSERT_EQUAL_UINT(BLOCK_FAIL, GetFromFbList(dieNo, GET_FREE_BLOCK_NORMAL));

	vsa = FindFreeVirtualSliceForGc(dieNo, fullBlock + 1);

	TEST_ASSERT_EQUAL_UINT(expected, Vsa2VblockTranslation(vsa));
	TEST_ASSERT_EQUAL_UINT(expected, virtualDieMapPtr->die[dieNo].currentBlock);
	TEST_ASSERT_EQUAL_UINT(0, virtualDieMapPtr->die[dieNo].freeBlockCnt);
}

static void test_gc_alloc_on_full_block_with_empty_list_asserts(void)
{
	const unsigned int dieNo = 0;
	unsigned int fullBlock = fill_current_block(dieNo);

	while (virtualDieMapPtr->die[dieNo].headFreeBlock != BLOCK_NONE)
		GetFromFbList(dieNo, GET_FREE_BLOCK_GC);

	FW_EXPECT_ASSERT(FindFreeVirtualSliceForGc(dieNo, fullBlock + 1));
}

static void test_gc_alloc_with_overflowed_page_counter_asserts(void)
{
	const unsigned int dieNo = 0;
	unsigned int blockNo = virtualDieMapPtr->die[dieNo].currentBlock;

	virtualBlockMapPtr->block[dieNo][blockNo].currentPage = USER_PAGES_PER_BLOCK + 1;

	FW_EXPECT_ASSERT(FindFreeVirtualSliceForGc(dieNo, blockNo + 1));
}

int main(void)
{
	UNITY_BEGIN();
	RUN_TEST(test_full_block_moves_to_next_free_block);
	RUN_TEST(test_allocation_advances_target_die);
	RUN_TEST(test_no_free_block_triggers_gc_and_reuses_freed_current_block);
	RUN_TEST(test_gc_that_refills_free_list_switches_current_block);
	RUN_TEST(test_gc_that_frees_nothing_asserts);
	RUN_TEST(test_gc_leaving_overflowed_page_counter_asserts);
	RUN_TEST(test_overflowed_current_page_asserts);
	RUN_TEST(test_gc_alloc_uses_current_block_of_target_die);
	RUN_TEST(test_gc_alloc_replaces_current_block_when_it_is_the_victim);
	RUN_TEST(test_gc_alloc_with_victim_current_block_and_empty_list_asserts);
	RUN_TEST(test_gc_alloc_on_full_block_takes_reserved_free_block);
	RUN_TEST(test_gc_alloc_on_full_block_with_empty_list_asserts);
	RUN_TEST(test_gc_alloc_with_overflowed_page_counter_asserts);
	return UNITY_END();
}
