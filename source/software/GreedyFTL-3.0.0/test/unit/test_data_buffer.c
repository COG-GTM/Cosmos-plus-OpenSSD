/* Unit tests for data_buffer.c (plus EvictDataBufEntry from request_transform.c). */
#include "unity.h"

#include "fw_test.h"
#include "memory_map.h"

/* data_buffer.h declares `extern ... dataBufHashTable`, but data_buffer.c
 * defines `dataBufHashTablePtr`; request_transform.h does not declare
 * EvictDataBufEntry. Declare both here. */
extern P_DATA_BUF_HASH_TABLE dataBufHashTablePtr;
void EvictDataBufEntry(unsigned int originReqSlotTag);

#define BUF_COUNT AVAILABLE_DATA_BUFFER_ENTRY_COUNT
#define LAST_BUF (BUF_COUNT - 1)

void setUp(void)
{
	fw_test_reset();
	fw_test_init_ftl();
}

void tearDown(void) {}

/* ---------- helpers ---------- */

static unsigned int NewReqForLsa(unsigned int logicalSliceAddr)
{
	unsigned int reqSlotTag = GetFromFreeReqQ();

	reqPoolPtr->reqPool[reqSlotTag].logicalSliceAddr = logicalSliceAddr;
	return reqSlotTag;
}

static void InsertBufWithLsa(unsigned int bufEntry, unsigned int logicalSliceAddr)
{
	dataBufMapPtr->dataBuf[bufEntry].logicalSliceAddr = logicalSliceAddr;
	PutToDataBufHashList(bufEntry);
}

/* Walk the LRU list head->tail and check it is well formed; return its length. */
static unsigned int LruListLength(void)
{
	unsigned int count = 0;
	unsigned int prev = DATA_BUF_NONE;
	unsigned int cur = dataBufLruList.headEntry;

	while (cur != DATA_BUF_NONE)
	{
		TEST_ASSERT_EQUAL_UINT(prev, dataBufMapPtr->dataBuf[cur].prevEntry);
		prev = cur;
		cur = dataBufMapPtr->dataBuf[cur].nextEntry;
		count++;
		TEST_ASSERT_TRUE_MESSAGE(count <= BUF_COUNT, "LRU list has a cycle");
	}
	TEST_ASSERT_EQUAL_UINT(prev, dataBufLruList.tailEntry);
	return count;
}

static unsigned int HashChainLength(unsigned int hashEntry)
{
	unsigned int count = 0;
	unsigned int prev = DATA_BUF_NONE;
	unsigned int cur = dataBufHashTablePtr->dataBufHash[hashEntry].headEntry;

	while (cur != DATA_BUF_NONE)
	{
		TEST_ASSERT_EQUAL_UINT(prev, dataBufMapPtr->dataBuf[cur].hashPrevEntry);
		prev = cur;
		cur = dataBufMapPtr->dataBuf[cur].hashNextEntry;
		count++;
		TEST_ASSERT_TRUE_MESSAGE(count <= BUF_COUNT, "hash chain has a cycle");
	}
	TEST_ASSERT_EQUAL_UINT(prev, dataBufHashTablePtr->dataBufHash[hashEntry].tailEntry);
	return count;
}

static unsigned int TotalNandReqCount(void)
{
	unsigned int total = 0;
	unsigned int ch, way;

	for (ch = 0; ch < USER_CHANNELS; ch++)
		for (way = 0; way < USER_WAYS; way++)
			total += nandReqQ[ch][way].reqCnt;
	return total;
}

/* ---------- InitDataBuf ---------- */

static void test_init_builds_full_lru_list_in_index_order(void)
{
	unsigned int bufEntry;

	TEST_ASSERT_EQUAL_UINT(0, dataBufLruList.headEntry);
	TEST_ASSERT_EQUAL_UINT(LAST_BUF, dataBufLruList.tailEntry);
	TEST_ASSERT_EQUAL_UINT(BUF_COUNT, LruListLength());

	for (bufEntry = 0; bufEntry < BUF_COUNT; bufEntry++)
	{
		TEST_ASSERT_EQUAL_HEX32(LSA_NONE, dataBufMapPtr->dataBuf[bufEntry].logicalSliceAddr);
		TEST_ASSERT_EQUAL_UINT(DATA_BUF_CLEAN, dataBufMapPtr->dataBuf[bufEntry].dirty);
		TEST_ASSERT_EQUAL_UINT(REQ_SLOT_TAG_NONE, dataBufMapPtr->dataBuf[bufEntry].blockingReqTail);
		TEST_ASSERT_EQUAL_UINT(DATA_BUF_NONE, dataBufMapPtr->dataBuf[bufEntry].hashPrevEntry);
		TEST_ASSERT_EQUAL_UINT(DATA_BUF_NONE, dataBufMapPtr->dataBuf[bufEntry].hashNextEntry);
		TEST_ASSERT_EQUAL_UINT(DATA_BUF_NONE, dataBufHashTablePtr->dataBufHash[bufEntry].headEntry);
		TEST_ASSERT_EQUAL_UINT(DATA_BUF_NONE, dataBufHashTablePtr->dataBufHash[bufEntry].tailEntry);
	}

	for (bufEntry = 0; bufEntry < AVAILABLE_TEMPORARY_DATA_BUFFER_ENTRY_COUNT; bufEntry++)
		TEST_ASSERT_EQUAL_UINT(REQ_SLOT_TAG_NONE, tempDataBufMapPtr->tempDataBuf[bufEntry].blockingReqTail);
}

/* ---------- CheckDataBufHit ---------- */

static void test_lookup_misses_on_empty_buffer(void)
{
	TEST_ASSERT_EQUAL_HEX32(DATA_BUF_FAIL, CheckDataBufHit(NewReqForLsa(42)));
	TEST_ASSERT_EQUAL_UINT(BUF_COUNT, LruListLength());
}

static void test_lookup_misses_when_hash_chain_has_only_other_lsas(void)
{
	/* Same hash bucket as 5, different LSA. */
	InsertBufWithLsa(3, 5 + BUF_COUNT);
	InsertBufWithLsa(7, 5 + 2 * BUF_COUNT);

	TEST_ASSERT_EQUAL_HEX32(DATA_BUF_FAIL, CheckDataBufHit(NewReqForLsa(5)));
	/* A miss must not disturb the LRU list. */
	TEST_ASSERT_EQUAL_UINT(0, dataBufLruList.headEntry);
	TEST_ASSERT_EQUAL_UINT(BUF_COUNT, LruListLength());
}

static void test_hit_on_middle_entry_moves_it_to_lru_head(void)
{
	unsigned int middle = BUF_COUNT / 2;

	InsertBufWithLsa(middle, 42);

	TEST_ASSERT_EQUAL_UINT(middle, CheckDataBufHit(NewReqForLsa(42)));

	TEST_ASSERT_EQUAL_UINT(middle, dataBufLruList.headEntry);
	TEST_ASSERT_EQUAL_UINT(0, dataBufMapPtr->dataBuf[middle].nextEntry);
	TEST_ASSERT_EQUAL_UINT(middle, dataBufMapPtr->dataBuf[0].prevEntry);
	TEST_ASSERT_EQUAL_UINT(middle + 1, dataBufMapPtr->dataBuf[middle - 1].nextEntry);
	TEST_ASSERT_EQUAL_UINT(middle - 1, dataBufMapPtr->dataBuf[middle + 1].prevEntry);
	TEST_ASSERT_EQUAL_UINT(LAST_BUF, dataBufLruList.tailEntry);
	TEST_ASSERT_EQUAL_UINT(BUF_COUNT, LruListLength());
}

static void test_hit_on_tail_entry_moves_it_to_lru_head(void)
{
	InsertBufWithLsa(LAST_BUF, 42);

	TEST_ASSERT_EQUAL_UINT(LAST_BUF, CheckDataBufHit(NewReqForLsa(42)));

	TEST_ASSERT_EQUAL_UINT(LAST_BUF, dataBufLruList.headEntry);
	TEST_ASSERT_EQUAL_UINT(LAST_BUF - 1, dataBufLruList.tailEntry);
	TEST_ASSERT_EQUAL_UINT(DATA_BUF_NONE, dataBufMapPtr->dataBuf[LAST_BUF - 1].nextEntry);
	TEST_ASSERT_EQUAL_UINT(BUF_COUNT, LruListLength());
}

static void test_hit_on_head_entry_keeps_it_at_lru_head(void)
{
	InsertBufWithLsa(0, 42);

	TEST_ASSERT_EQUAL_UINT(0, CheckDataBufHit(NewReqForLsa(42)));

	TEST_ASSERT_EQUAL_UINT(0, dataBufLruList.headEntry);
	TEST_ASSERT_EQUAL_UINT(1, dataBufMapPtr->dataBuf[0].nextEntry);
	TEST_ASSERT_EQUAL_UINT(DATA_BUF_NONE, dataBufMapPtr->dataBuf[0].prevEntry);
	TEST_ASSERT_EQUAL_UINT(LAST_BUF, dataBufLruList.tailEntry);
	TEST_ASSERT_EQUAL_UINT(BUF_COUNT, LruListLength());
}

static void test_hit_on_sole_lru_entry_keeps_single_entry_list(void)
{
	/* Shrink the LRU list to a single entry (0). */
	dataBufMapPtr->dataBuf[0].nextEntry = DATA_BUF_NONE;
	dataBufLruList.tailEntry = 0;
	InsertBufWithLsa(0, 42);

	TEST_ASSERT_EQUAL_UINT(0, CheckDataBufHit(NewReqForLsa(42)));

	TEST_ASSERT_EQUAL_UINT(0, dataBufLruList.headEntry);
	TEST_ASSERT_EQUAL_UINT(0, dataBufLruList.tailEntry);
	TEST_ASSERT_EQUAL_UINT(DATA_BUF_NONE, dataBufMapPtr->dataBuf[0].prevEntry);
	TEST_ASSERT_EQUAL_UINT(DATA_BUF_NONE, dataBufMapPtr->dataBuf[0].nextEntry);
	TEST_ASSERT_EQUAL_UINT(1, LruListLength());
}

static void test_hit_walks_hash_chain_past_colliding_entries(void)
{
	unsigned int lsa = 9;

	InsertBufWithLsa(2, lsa + BUF_COUNT);
	InsertBufWithLsa(4, lsa + 2 * BUF_COUNT);
	InsertBufWithLsa(6, lsa);

	TEST_ASSERT_EQUAL_UINT(6, CheckDataBufHit(NewReqForLsa(lsa)));
	TEST_ASSERT_EQUAL_UINT(4, CheckDataBufHit(NewReqForLsa(lsa + 2 * BUF_COUNT)));
	TEST_ASSERT_EQUAL_UINT(2, CheckDataBufHit(NewReqForLsa(lsa + BUF_COUNT)));

	/* Most recently hit is at head; order is 2, 4, 6, ... */
	TEST_ASSERT_EQUAL_UINT(2, dataBufLruList.headEntry);
	TEST_ASSERT_EQUAL_UINT(4, dataBufMapPtr->dataBuf[2].nextEntry);
	TEST_ASSERT_EQUAL_UINT(6, dataBufMapPtr->dataBuf[4].nextEntry);
	TEST_ASSERT_EQUAL_UINT(BUF_COUNT, LruListLength());
}

/* ---------- AllocateDataBuf ---------- */

static void test_allocate_takes_lru_tail_and_moves_it_to_head(void)
{
	unsigned int bufEntry = AllocateDataBuf();

	TEST_ASSERT_EQUAL_UINT(LAST_BUF, bufEntry);
	TEST_ASSERT_EQUAL_UINT(LAST_BUF, dataBufLruList.headEntry);
	TEST_ASSERT_EQUAL_UINT(LAST_BUF - 1, dataBufLruList.tailEntry);
	TEST_ASSERT_EQUAL_UINT(0, dataBufMapPtr->dataBuf[LAST_BUF].nextEntry);
	TEST_ASSERT_EQUAL_UINT(DATA_BUF_NONE, dataBufMapPtr->dataBuf[LAST_BUF].prevEntry);
	TEST_ASSERT_EQUAL_UINT(DATA_BUF_NONE, dataBufMapPtr->dataBuf[LAST_BUF - 1].nextEntry);
	TEST_ASSERT_EQUAL_UINT(BUF_COUNT, LruListLength());
}

static void test_repeated_allocation_cycles_through_every_entry(void)
{
	unsigned int i;

	for (i = 0; i < BUF_COUNT; i++)
		TEST_ASSERT_EQUAL_UINT(LAST_BUF - i, AllocateDataBuf());

	/* One full rotation restores the initial order. */
	TEST_ASSERT_EQUAL_UINT(0, dataBufLruList.headEntry);
	TEST_ASSERT_EQUAL_UINT(LAST_BUF, dataBufLruList.tailEntry);
	TEST_ASSERT_EQUAL_UINT(BUF_COUNT, LruListLength());
	TEST_ASSERT_EQUAL_UINT(LAST_BUF, AllocateDataBuf());
}

static void test_allocate_prefers_least_recently_hit_entry(void)
{
	InsertBufWithLsa(LAST_BUF, 42);
	TEST_ASSERT_EQUAL_UINT(LAST_BUF, CheckDataBufHit(NewReqForLsa(42)));

	TEST_ASSERT_EQUAL_UINT(LAST_BUF - 1, AllocateDataBuf());
	TEST_ASSERT_EQUAL_UINT(BUF_COUNT, LruListLength());
}

static void test_allocate_removes_victim_from_hash_list(void)
{
	unsigned int lsa = 42;
	unsigned int hashEntry = FindDataBufHashTableEntry(lsa);

	InsertBufWithLsa(LAST_BUF, lsa);
	TEST_ASSERT_EQUAL_UINT(1, HashChainLength(hashEntry));

	TEST_ASSERT_EQUAL_UINT(LAST_BUF, AllocateDataBuf());

	TEST_ASSERT_EQUAL_UINT(0, HashChainLength(hashEntry));
	TEST_ASSERT_EQUAL_UINT(DATA_BUF_NONE, dataBufHashTablePtr->dataBufHash[hashEntry].headEntry);
	TEST_ASSERT_EQUAL_HEX32(DATA_BUF_FAIL, CheckDataBufHit(NewReqForLsa(lsa)));
}

static void test_allocate_with_single_entry_list_keeps_that_entry(void)
{
	dataBufMapPtr->dataBuf[0].nextEntry = DATA_BUF_NONE;
	dataBufLruList.tailEntry = 0;

	TEST_ASSERT_EQUAL_UINT(0, AllocateDataBuf());
	TEST_ASSERT_EQUAL_UINT(0, dataBufLruList.headEntry);
	TEST_ASSERT_EQUAL_UINT(0, dataBufLruList.tailEntry);
	TEST_ASSERT_EQUAL_UINT(1, LruListLength());

	TEST_ASSERT_EQUAL_UINT(0, AllocateDataBuf());
	TEST_ASSERT_EQUAL_UINT(1, LruListLength());
}

static void test_allocate_asserts_when_lru_list_is_empty(void)
{
	dataBufLruList.headEntry = DATA_BUF_NONE;
	dataBufLruList.tailEntry = DATA_BUF_NONE;

	FW_EXPECT_ASSERT(AllocateDataBuf());
}

/* ---------- blocking request chains ---------- */

static void test_first_blocking_req_becomes_tail_without_linking(void)
{
	unsigned int reqSlotTag = GetFromFreeReqQ();

	UpdateDataBufEntryInfoBlockingReq(3, reqSlotTag);

	TEST_ASSERT_EQUAL_UINT(reqSlotTag, dataBufMapPtr->dataBuf[3].blockingReqTail);
	TEST_ASSERT_EQUAL_UINT(REQ_SLOT_TAG_NONE, reqPoolPtr->reqPool[reqSlotTag].prevBlockingReq);
	TEST_ASSERT_EQUAL_UINT(REQ_SLOT_TAG_NONE, reqPoolPtr->reqPool[reqSlotTag].nextBlockingReq);
}

static void test_later_blocking_reqs_are_chained_in_fifo_order(void)
{
	unsigned int first = GetFromFreeReqQ();
	unsigned int second = GetFromFreeReqQ();
	unsigned int third = GetFromFreeReqQ();

	UpdateDataBufEntryInfoBlockingReq(3, first);
	UpdateDataBufEntryInfoBlockingReq(3, second);
	UpdateDataBufEntryInfoBlockingReq(3, third);

	TEST_ASSERT_EQUAL_UINT(third, dataBufMapPtr->dataBuf[3].blockingReqTail);
	TEST_ASSERT_EQUAL_UINT(second, reqPoolPtr->reqPool[first].nextBlockingReq);
	TEST_ASSERT_EQUAL_UINT(first, reqPoolPtr->reqPool[second].prevBlockingReq);
	TEST_ASSERT_EQUAL_UINT(third, reqPoolPtr->reqPool[second].nextBlockingReq);
	TEST_ASSERT_EQUAL_UINT(second, reqPoolPtr->reqPool[third].prevBlockingReq);
	TEST_ASSERT_EQUAL_UINT(REQ_SLOT_TAG_NONE, reqPoolPtr->reqPool[third].nextBlockingReq);
	TEST_ASSERT_EQUAL_UINT(REQ_SLOT_TAG_NONE, reqPoolPtr->reqPool[first].prevBlockingReq);
}

static void test_blocking_chains_are_independent_per_buffer(void)
{
	unsigned int reqA = GetFromFreeReqQ();
	unsigned int reqB = GetFromFreeReqQ();

	UpdateDataBufEntryInfoBlockingReq(1, reqA);
	UpdateDataBufEntryInfoBlockingReq(2, reqB);

	TEST_ASSERT_EQUAL_UINT(reqA, dataBufMapPtr->dataBuf[1].blockingReqTail);
	TEST_ASSERT_EQUAL_UINT(reqB, dataBufMapPtr->dataBuf[2].blockingReqTail);
	TEST_ASSERT_EQUAL_UINT(REQ_SLOT_TAG_NONE, reqPoolPtr->reqPool[reqA].nextBlockingReq);
	TEST_ASSERT_EQUAL_UINT(REQ_SLOT_TAG_NONE, reqPoolPtr->reqPool[reqB].prevBlockingReq);
}

/* ---------- temporary data buffers ---------- */

static void test_temp_buffer_is_indexed_by_die(void)
{
	unsigned int dieNo;

	for (dieNo = 0; dieNo < USER_DIES; dieNo++)
		TEST_ASSERT_EQUAL_UINT(dieNo, AllocateTempDataBuf(dieNo));
}

static void test_temp_buffer_blocking_reqs_are_chained(void)
{
	unsigned int first = GetFromFreeReqQ();
	unsigned int second = GetFromFreeReqQ();
	unsigned int tempEntry = AllocateTempDataBuf(USER_DIES - 1);

	UpdateTempDataBufEntryInfoBlockingReq(tempEntry, first);
	TEST_ASSERT_EQUAL_UINT(first, tempDataBufMapPtr->tempDataBuf[tempEntry].blockingReqTail);
	TEST_ASSERT_EQUAL_UINT(REQ_SLOT_TAG_NONE, reqPoolPtr->reqPool[first].prevBlockingReq);

	UpdateTempDataBufEntryInfoBlockingReq(tempEntry, second);
	TEST_ASSERT_EQUAL_UINT(second, tempDataBufMapPtr->tempDataBuf[tempEntry].blockingReqTail);
	TEST_ASSERT_EQUAL_UINT(first, reqPoolPtr->reqPool[second].prevBlockingReq);
	TEST_ASSERT_EQUAL_UINT(second, reqPoolPtr->reqPool[first].nextBlockingReq);

	/* Temp and regular buffer chains do not interfere. */
	TEST_ASSERT_EQUAL_UINT(REQ_SLOT_TAG_NONE, dataBufMapPtr->dataBuf[tempEntry].blockingReqTail);
}

/* ---------- hash list insert / remove ---------- */

static void test_put_into_empty_bucket_sets_head_and_tail(void)
{
	unsigned int hashEntry = FindDataBufHashTableEntry(42);

	InsertBufWithLsa(5, 42);

	TEST_ASSERT_EQUAL_UINT(5, dataBufHashTablePtr->dataBufHash[hashEntry].headEntry);
	TEST_ASSERT_EQUAL_UINT(5, dataBufHashTablePtr->dataBufHash[hashEntry].tailEntry);
	TEST_ASSERT_EQUAL_UINT(DATA_BUF_NONE, dataBufMapPtr->dataBuf[5].hashPrevEntry);
	TEST_ASSERT_EQUAL_UINT(DATA_BUF_NONE, dataBufMapPtr->dataBuf[5].hashNextEntry);
	TEST_ASSERT_EQUAL_UINT(1, HashChainLength(hashEntry));
}

static void test_put_appends_colliding_entries_to_bucket_tail(void)
{
	unsigned int hashEntry = FindDataBufHashTableEntry(1);

	InsertBufWithLsa(5, 1);
	InsertBufWithLsa(6, 1 + BUF_COUNT);
	InsertBufWithLsa(7, 1 + 2 * BUF_COUNT);

	TEST_ASSERT_EQUAL_UINT(5, dataBufHashTablePtr->dataBufHash[hashEntry].headEntry);
	TEST_ASSERT_EQUAL_UINT(7, dataBufHashTablePtr->dataBufHash[hashEntry].tailEntry);
	TEST_ASSERT_EQUAL_UINT(6, dataBufMapPtr->dataBuf[5].hashNextEntry);
	TEST_ASSERT_EQUAL_UINT(5, dataBufMapPtr->dataBuf[6].hashPrevEntry);
	TEST_ASSERT_EQUAL_UINT(7, dataBufMapPtr->dataBuf[6].hashNextEntry);
	TEST_ASSERT_EQUAL_UINT(6, dataBufMapPtr->dataBuf[7].hashPrevEntry);
	TEST_ASSERT_EQUAL_UINT(3, HashChainLength(hashEntry));
}

static void test_put_uses_distinct_buckets_for_distinct_residues(void)
{
	InsertBufWithLsa(5, 1);
	InsertBufWithLsa(6, 2);

	TEST_ASSERT_EQUAL_UINT(1, HashChainLength(FindDataBufHashTableEntry(1)));
	TEST_ASSERT_EQUAL_UINT(1, HashChainLength(FindDataBufHashTableEntry(2)));
	TEST_ASSERT_EQUAL_UINT(DATA_BUF_NONE, dataBufMapPtr->dataBuf[5].hashNextEntry);
}

static void test_remove_ignores_entry_without_lsa(void)
{
	unsigned int hashEntry = FindDataBufHashTableEntry(1);

	InsertBufWithLsa(5, 1);
	/* Entry 9 has LSA_NONE and is not in any bucket. */
	SelectiveGetFromDataBufHashList(9);

	TEST_ASSERT_EQUAL_UINT(1, HashChainLength(hashEntry));
	TEST_ASSERT_EQUAL_UINT(5, dataBufHashTablePtr->dataBufHash[hashEntry].headEntry);
}

static void test_remove_middle_of_chain_relinks_neighbours(void)
{
	unsigned int hashEntry = FindDataBufHashTableEntry(1);

	InsertBufWithLsa(5, 1);
	InsertBufWithLsa(6, 1 + BUF_COUNT);
	InsertBufWithLsa(7, 1 + 2 * BUF_COUNT);

	SelectiveGetFromDataBufHashList(6);

	TEST_ASSERT_EQUAL_UINT(7, dataBufMapPtr->dataBuf[5].hashNextEntry);
	TEST_ASSERT_EQUAL_UINT(5, dataBufMapPtr->dataBuf[7].hashPrevEntry);
	TEST_ASSERT_EQUAL_UINT(5, dataBufHashTablePtr->dataBufHash[hashEntry].headEntry);
	TEST_ASSERT_EQUAL_UINT(7, dataBufHashTablePtr->dataBufHash[hashEntry].tailEntry);
	TEST_ASSERT_EQUAL_UINT(2, HashChainLength(hashEntry));
}

static void test_remove_tail_of_chain_updates_bucket_tail(void)
{
	unsigned int hashEntry = FindDataBufHashTableEntry(1);

	InsertBufWithLsa(5, 1);
	InsertBufWithLsa(6, 1 + BUF_COUNT);

	SelectiveGetFromDataBufHashList(6);

	TEST_ASSERT_EQUAL_UINT(DATA_BUF_NONE, dataBufMapPtr->dataBuf[5].hashNextEntry);
	TEST_ASSERT_EQUAL_UINT(5, dataBufHashTablePtr->dataBufHash[hashEntry].headEntry);
	TEST_ASSERT_EQUAL_UINT(5, dataBufHashTablePtr->dataBufHash[hashEntry].tailEntry);
	TEST_ASSERT_EQUAL_UINT(1, HashChainLength(hashEntry));
}

static void test_remove_head_of_chain_updates_bucket_head(void)
{
	unsigned int hashEntry = FindDataBufHashTableEntry(1);

	InsertBufWithLsa(5, 1);
	InsertBufWithLsa(6, 1 + BUF_COUNT);

	SelectiveGetFromDataBufHashList(5);

	TEST_ASSERT_EQUAL_UINT(DATA_BUF_NONE, dataBufMapPtr->dataBuf[6].hashPrevEntry);
	TEST_ASSERT_EQUAL_UINT(6, dataBufHashTablePtr->dataBufHash[hashEntry].headEntry);
	TEST_ASSERT_EQUAL_UINT(6, dataBufHashTablePtr->dataBufHash[hashEntry].tailEntry);
	TEST_ASSERT_EQUAL_UINT(1, HashChainLength(hashEntry));
}

static void test_remove_sole_entry_empties_bucket(void)
{
	unsigned int hashEntry = FindDataBufHashTableEntry(1);

	InsertBufWithLsa(5, 1);
	SelectiveGetFromDataBufHashList(5);

	TEST_ASSERT_EQUAL_UINT(DATA_BUF_NONE, dataBufHashTablePtr->dataBufHash[hashEntry].headEntry);
	TEST_ASSERT_EQUAL_UINT(DATA_BUF_NONE, dataBufHashTablePtr->dataBufHash[hashEntry].tailEntry);
	TEST_ASSERT_EQUAL_UINT(0, HashChainLength(hashEntry));
}

static void test_remove_then_reinsert_with_new_lsa_is_found(void)
{
	InsertBufWithLsa(5, 1);
	SelectiveGetFromDataBufHashList(5);
	InsertBufWithLsa(5, 77);

	TEST_ASSERT_EQUAL_HEX32(DATA_BUF_FAIL, CheckDataBufHit(NewReqForLsa(1)));
	TEST_ASSERT_EQUAL_UINT(5, CheckDataBufHit(NewReqForLsa(77)));
}

/* ---------- EvictDataBufEntry (request_transform.c) ---------- */

static unsigned int PrepareOriginReq(unsigned int bufEntry, unsigned int lsa, unsigned int dirty)
{
	unsigned int originReq = NewReqForLsa(lsa);

	reqPoolPtr->reqPool[originReq].nvmeCmdSlotTag = 3;
	reqPoolPtr->reqPool[originReq].dataBufInfo.entry = bufEntry;
	InsertBufWithLsa(bufEntry, lsa);
	dataBufMapPtr->dataBuf[bufEntry].dirty = dirty;
	return originReq;
}

static void test_evict_clean_entry_issues_no_request(void)
{
	unsigned int bufEntry = AllocateDataBuf();
	unsigned int originReq = PrepareOriginReq(bufEntry, 42, DATA_BUF_CLEAN);
	unsigned int freeBefore = freeReqQ.reqCnt;

	EvictDataBufEntry(originReq);

	TEST_ASSERT_EQUAL_UINT(freeBefore, freeReqQ.reqCnt);
	TEST_ASSERT_EQUAL_UINT(0, TotalNandReqCount());
	TEST_ASSERT_EQUAL_UINT(REQ_SLOT_TAG_NONE, dataBufMapPtr->dataBuf[bufEntry].blockingReqTail);
	TEST_ASSERT_EQUAL_UINT(DATA_BUF_CLEAN, dataBufMapPtr->dataBuf[bufEntry].dirty);
}

static void test_evict_dirty_entry_issues_nand_write_and_clears_dirty(void)
{
	unsigned int lsa = 42;
	unsigned int bufEntry = AllocateDataBuf();
	unsigned int originReq = PrepareOriginReq(bufEntry, lsa, DATA_BUF_DIRTY);
	unsigned int freeBefore = freeReqQ.reqCnt;
	unsigned int writeReq;

	EvictDataBufEntry(originReq);

	TEST_ASSERT_EQUAL_UINT(freeBefore - 1, freeReqQ.reqCnt);
	TEST_ASSERT_EQUAL_UINT(1, TotalNandReqCount());
	TEST_ASSERT_EQUAL_UINT(DATA_BUF_CLEAN, dataBufMapPtr->dataBuf[bufEntry].dirty);

	writeReq = dataBufMapPtr->dataBuf[bufEntry].blockingReqTail;
	TEST_ASSERT_NOT_EQUAL(REQ_SLOT_TAG_NONE, writeReq);
	TEST_ASSERT_NOT_EQUAL(originReq, writeReq);
	TEST_ASSERT_EQUAL_UINT(REQ_TYPE_NAND, reqPoolPtr->reqPool[writeReq].reqType);
	TEST_ASSERT_EQUAL_UINT(REQ_CODE_WRITE, reqPoolPtr->reqPool[writeReq].reqCode);
	TEST_ASSERT_EQUAL_UINT(3, reqPoolPtr->reqPool[writeReq].nvmeCmdSlotTag);
	TEST_ASSERT_EQUAL_UINT(lsa, reqPoolPtr->reqPool[writeReq].logicalSliceAddr);
	TEST_ASSERT_EQUAL_UINT(bufEntry, reqPoolPtr->reqPool[writeReq].dataBufInfo.entry);
	TEST_ASSERT_EQUAL_UINT(REQ_OPT_DATA_BUF_ENTRY, reqPoolPtr->reqPool[writeReq].reqOpt.dataBufFormat);
	TEST_ASSERT_EQUAL_UINT(REQ_OPT_NAND_ADDR_VSA, reqPoolPtr->reqPool[writeReq].reqOpt.nandAddr);
	TEST_ASSERT_EQUAL_UINT(REQ_OPT_NAND_ECC_ON, reqPoolPtr->reqPool[writeReq].reqOpt.nandEcc);
	TEST_ASSERT_EQUAL_UINT(REQ_OPT_NAND_ECC_WARNING_ON, reqPoolPtr->reqPool[writeReq].reqOpt.nandEccWarning);
	TEST_ASSERT_EQUAL_UINT(REQ_OPT_ROW_ADDR_DEPENDENCY_CHECK, reqPoolPtr->reqPool[writeReq].reqOpt.rowAddrDependencyCheck);
	TEST_ASSERT_EQUAL_UINT(REQ_OPT_BLOCK_SPACE_MAIN, reqPoolPtr->reqPool[writeReq].reqOpt.blockSpace);

	/* The write-back allocated a fresh VSA and mapped the LSA to it. */
	TEST_ASSERT_NOT_EQUAL(VSA_NONE, reqPoolPtr->reqPool[writeReq].nandInfo.virtualSliceAddr);
	TEST_ASSERT_EQUAL_HEX32(reqPoolPtr->reqPool[writeReq].nandInfo.virtualSliceAddr, AddrTransRead(lsa));
}

static void test_evict_dirty_entry_appends_write_to_existing_blocking_chain(void)
{
	unsigned int bufEntry = AllocateDataBuf();
	unsigned int originReq = PrepareOriginReq(bufEntry, 42, DATA_BUF_DIRTY);
	unsigned int earlierReq = GetFromFreeReqQ();
	unsigned int writeReq;

	UpdateDataBufEntryInfoBlockingReq(bufEntry, earlierReq);

	EvictDataBufEntry(originReq);

	writeReq = dataBufMapPtr->dataBuf[bufEntry].blockingReqTail;
	TEST_ASSERT_NOT_EQUAL(earlierReq, writeReq);
	TEST_ASSERT_EQUAL_UINT(earlierReq, reqPoolPtr->reqPool[writeReq].prevBlockingReq);
	TEST_ASSERT_EQUAL_UINT(writeReq, reqPoolPtr->reqPool[earlierReq].nextBlockingReq);

	/* Blocked by the buffer dependency: parked, not dispatched to NAND. */
	TEST_ASSERT_EQUAL_UINT(0, TotalNandReqCount());
	TEST_ASSERT_EQUAL_UINT(1, blockedByBufDepReqQ.reqCnt);
	TEST_ASSERT_EQUAL_UINT(DATA_BUF_CLEAN, dataBufMapPtr->dataBuf[bufEntry].dirty);
}

static void test_evict_twice_only_writes_back_once(void)
{
	unsigned int bufEntry = AllocateDataBuf();
	unsigned int originReq = PrepareOriginReq(bufEntry, 42, DATA_BUF_DIRTY);

	EvictDataBufEntry(originReq);
	EvictDataBufEntry(originReq);

	TEST_ASSERT_EQUAL_UINT(1, TotalNandReqCount());
	TEST_ASSERT_EQUAL_UINT(1, notCompletedNandReqCnt);
}

int main(void)
{
	UNITY_BEGIN();
	RUN_TEST(test_init_builds_full_lru_list_in_index_order);

	RUN_TEST(test_lookup_misses_on_empty_buffer);
	RUN_TEST(test_lookup_misses_when_hash_chain_has_only_other_lsas);
	RUN_TEST(test_hit_on_middle_entry_moves_it_to_lru_head);
	RUN_TEST(test_hit_on_tail_entry_moves_it_to_lru_head);
	RUN_TEST(test_hit_on_head_entry_keeps_it_at_lru_head);
	RUN_TEST(test_hit_on_sole_lru_entry_keeps_single_entry_list);
	RUN_TEST(test_hit_walks_hash_chain_past_colliding_entries);

	RUN_TEST(test_allocate_takes_lru_tail_and_moves_it_to_head);
	RUN_TEST(test_repeated_allocation_cycles_through_every_entry);
	RUN_TEST(test_allocate_prefers_least_recently_hit_entry);
	RUN_TEST(test_allocate_removes_victim_from_hash_list);
	RUN_TEST(test_allocate_with_single_entry_list_keeps_that_entry);
	RUN_TEST(test_allocate_asserts_when_lru_list_is_empty);

	RUN_TEST(test_first_blocking_req_becomes_tail_without_linking);
	RUN_TEST(test_later_blocking_reqs_are_chained_in_fifo_order);
	RUN_TEST(test_blocking_chains_are_independent_per_buffer);

	RUN_TEST(test_temp_buffer_is_indexed_by_die);
	RUN_TEST(test_temp_buffer_blocking_reqs_are_chained);

	RUN_TEST(test_put_into_empty_bucket_sets_head_and_tail);
	RUN_TEST(test_put_appends_colliding_entries_to_bucket_tail);
	RUN_TEST(test_put_uses_distinct_buckets_for_distinct_residues);
	RUN_TEST(test_remove_ignores_entry_without_lsa);
	RUN_TEST(test_remove_middle_of_chain_relinks_neighbours);
	RUN_TEST(test_remove_tail_of_chain_updates_bucket_tail);
	RUN_TEST(test_remove_head_of_chain_updates_bucket_head);
	RUN_TEST(test_remove_sole_entry_empties_bucket);
	RUN_TEST(test_remove_then_reinsert_with_new_lsa_is_found);

	RUN_TEST(test_evict_clean_entry_issues_no_request);
	RUN_TEST(test_evict_dirty_entry_issues_nand_write_and_clears_dirty);
	RUN_TEST(test_evict_dirty_entry_appends_write_to_existing_blocking_chain);
	RUN_TEST(test_evict_twice_only_writes_back_once);
	return UNITY_END();
}
