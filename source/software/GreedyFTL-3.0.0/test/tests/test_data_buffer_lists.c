/*
 * Data-buffer LRU / hash-chain unlink paths that the I/O-level tests do not
 * reach: hits on the LRU tail, LRU head, and a single-entry LRU list; hash
 * chain removal of the head, middle, and tail entry; and the "no buffer left"
 * assertion in AllocateDataBuf().
 */
#include "unity.h"

#include "ftl_test_env.h"

#define ENTRIES AVAILABLE_DATA_BUFFER_ENTRY_COUNT

void setUp(void)
{
	ftl_test_env_init();
}

void tearDown(void)
{
}

/* Take the LRU tail, bind it to lsa and publish it in the hash table. */
static unsigned int Claim(unsigned int lsa)
{
	unsigned int entry = AllocateDataBuf();
	dataBufMapPtr->dataBuf[entry].logicalSliceAddr = lsa;
	PutToDataBufHashList(entry);
	return entry;
}

static unsigned int HitLookup(unsigned int lsa)
{
	unsigned int tag = GetFromFreeReqQ();
	unsigned int entry;
	reqPoolPtr->reqPool[tag].logicalSliceAddr = lsa;
	entry = CheckDataBufHit(tag);
	PutToFreeReqQ(tag);
	return entry;
}

static void AssertLruIsConsistent(void)
{
	unsigned int entry = dataBufLruList.headEntry, prev = DATA_BUF_NONE;
	int n = 0;
	while (entry != DATA_BUF_NONE)
	{
		TEST_ASSERT_EQUAL_UINT32(prev, dataBufMapPtr->dataBuf[entry].prevEntry);
		prev = entry;
		entry = dataBufMapPtr->dataBuf[entry].nextEntry;
		n++;
	}
	TEST_ASSERT_EQUAL_UINT32(prev, dataBufLruList.tailEntry);
	TEST_ASSERT_EQUAL_INT(ENTRIES, n);
}

/* ---- LRU unlink on hit --------------------------------------------------- */

static void test_hit_on_lru_head_keeps_it_at_head(void)
{
	unsigned int entry = Claim(7);

	TEST_ASSERT_EQUAL_UINT32(entry, dataBufLruList.headEntry);
	TEST_ASSERT_EQUAL_UINT32(entry, HitLookup(7));
	TEST_ASSERT_EQUAL_UINT32(entry, dataBufLruList.headEntry);
	AssertLruIsConsistent();
}

static void test_hit_on_lru_tail_moves_it_to_head(void)
{
	unsigned int first = Claim(0), i;

	for (i = 1; i < ENTRIES; i++)
		Claim(i);
	TEST_ASSERT_EQUAL_UINT32(first, dataBufLruList.tailEntry);

	TEST_ASSERT_EQUAL_UINT32(first, HitLookup(0));

	TEST_ASSERT_EQUAL_UINT32(first, dataBufLruList.headEntry);
	TEST_ASSERT_NOT_EQUAL(first, dataBufLruList.tailEntry);
	AssertLruIsConsistent();
}

static void test_hit_on_only_lru_entry_leaves_single_entry_list(void)
{
	unsigned int entry = Claim(5);

	/* Shrink the LRU list to this single entry. */
	dataBufMapPtr->dataBuf[entry].prevEntry = DATA_BUF_NONE;
	dataBufMapPtr->dataBuf[entry].nextEntry = DATA_BUF_NONE;
	dataBufLruList.headEntry = entry;
	dataBufLruList.tailEntry = entry;

	TEST_ASSERT_EQUAL_UINT32(entry, HitLookup(5));

	TEST_ASSERT_EQUAL_UINT32(entry, dataBufLruList.headEntry);
	TEST_ASSERT_EQUAL_UINT32(entry, dataBufLruList.tailEntry);
	TEST_ASSERT_EQUAL_UINT32(DATA_BUF_NONE, dataBufMapPtr->dataBuf[entry].prevEntry);
	TEST_ASSERT_EQUAL_UINT32(DATA_BUF_NONE, dataBufMapPtr->dataBuf[entry].nextEntry);
}

static void test_allocating_only_lru_entry_keeps_it_as_head_and_tail(void)
{
	unsigned int entry = dataBufLruList.tailEntry;

	dataBufMapPtr->dataBuf[entry].prevEntry = DATA_BUF_NONE;
	dataBufMapPtr->dataBuf[entry].nextEntry = DATA_BUF_NONE;
	dataBufLruList.headEntry = entry;

	TEST_ASSERT_EQUAL_UINT32(entry, AllocateDataBuf());
	TEST_ASSERT_EQUAL_UINT32(entry, dataBufLruList.headEntry);
	TEST_ASSERT_EQUAL_UINT32(entry, dataBufLruList.tailEntry);
}

static void ExhaustedAllocate(void)
{
	dataBufLruList.tailEntry = DATA_BUF_NONE;
	AllocateDataBuf();
}

static void test_allocate_with_empty_lru_list_asserts(void)
{
	TEST_ASSERT_TRUE(ftl_test_expect_abort(ExhaustedAllocate));
}

/* ---- Hash chain removal -------------------------------------------------- */

#define COLLIDE(n) ((n) * ENTRIES + 3)

static void test_removing_middle_of_hash_chain_relinks_neighbours(void)
{
	unsigned int a = Claim(COLLIDE(0)), b = Claim(COLLIDE(1)), c = Claim(COLLIDE(2));
	unsigned int hash = FindDataBufHashTableEntry(COLLIDE(0));

	TEST_ASSERT_EQUAL_UINT32(a, dataBufHashTablePtr->dataBufHash[hash].headEntry);
	TEST_ASSERT_EQUAL_UINT32(c, dataBufHashTablePtr->dataBufHash[hash].tailEntry);

	SelectiveGetFromDataBufHashList(b);

	TEST_ASSERT_EQUAL_UINT32(c, dataBufMapPtr->dataBuf[a].hashNextEntry);
	TEST_ASSERT_EQUAL_UINT32(a, dataBufMapPtr->dataBuf[c].hashPrevEntry);
	TEST_ASSERT_EQUAL_UINT32(a, dataBufHashTablePtr->dataBufHash[hash].headEntry);
	TEST_ASSERT_EQUAL_UINT32(c, dataBufHashTablePtr->dataBufHash[hash].tailEntry);
	TEST_ASSERT_EQUAL_UINT32(DATA_BUF_FAIL, HitLookup(COLLIDE(1)));
	TEST_ASSERT_EQUAL_UINT32(a, HitLookup(COLLIDE(0)));
	TEST_ASSERT_EQUAL_UINT32(c, HitLookup(COLLIDE(2)));
}

static void test_removing_tail_of_hash_chain_updates_tail(void)
{
	unsigned int a = Claim(COLLIDE(0)), b = Claim(COLLIDE(1));
	unsigned int hash = FindDataBufHashTableEntry(COLLIDE(0));

	SelectiveGetFromDataBufHashList(b);

	TEST_ASSERT_EQUAL_UINT32(a, dataBufHashTablePtr->dataBufHash[hash].headEntry);
	TEST_ASSERT_EQUAL_UINT32(a, dataBufHashTablePtr->dataBufHash[hash].tailEntry);
	TEST_ASSERT_EQUAL_UINT32(DATA_BUF_NONE, dataBufMapPtr->dataBuf[a].hashNextEntry);
}

static void test_removing_head_of_hash_chain_updates_head(void)
{
	unsigned int a = Claim(COLLIDE(0)), b = Claim(COLLIDE(1));
	unsigned int hash = FindDataBufHashTableEntry(COLLIDE(0));

	SelectiveGetFromDataBufHashList(a);

	TEST_ASSERT_EQUAL_UINT32(b, dataBufHashTablePtr->dataBufHash[hash].headEntry);
	TEST_ASSERT_EQUAL_UINT32(b, dataBufHashTablePtr->dataBufHash[hash].tailEntry);
	TEST_ASSERT_EQUAL_UINT32(DATA_BUF_NONE, dataBufMapPtr->dataBuf[b].hashPrevEntry);
}

static void test_removing_only_entry_empties_hash_chain(void)
{
	unsigned int a = Claim(COLLIDE(0));
	unsigned int hash = FindDataBufHashTableEntry(COLLIDE(0));

	SelectiveGetFromDataBufHashList(a);

	TEST_ASSERT_EQUAL_UINT32(DATA_BUF_NONE, dataBufHashTablePtr->dataBufHash[hash].headEntry);
	TEST_ASSERT_EQUAL_UINT32(DATA_BUF_NONE, dataBufHashTablePtr->dataBufHash[hash].tailEntry);
	TEST_ASSERT_EQUAL_UINT32(DATA_BUF_FAIL, HitLookup(COLLIDE(0)));
}

int main(void)
{
	UNITY_BEGIN();
	RUN_TEST(test_hit_on_lru_head_keeps_it_at_head);
	RUN_TEST(test_hit_on_lru_tail_moves_it_to_head);
	RUN_TEST(test_hit_on_only_lru_entry_leaves_single_entry_list);
	RUN_TEST(test_allocating_only_lru_entry_keeps_it_as_head_and_tail);
	RUN_TEST(test_allocate_with_empty_lru_list_asserts);
	RUN_TEST(test_removing_middle_of_hash_chain_relinks_neighbours);
	RUN_TEST(test_removing_tail_of_hash_chain_updates_tail);
	RUN_TEST(test_removing_head_of_hash_chain_updates_head);
	RUN_TEST(test_removing_only_entry_empties_hash_chain);
	return UNITY_END();
}
