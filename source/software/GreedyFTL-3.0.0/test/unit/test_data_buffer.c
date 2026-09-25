/* Unit tests for data_buffer.c: LRU list maintenance, hash-list lookup,
 * hit/miss handling, blocking-request chaining, temp buffers and the
 * dirty-eviction path in request_transform.c (EvictDataBufEntry). */
#include "unity.h"
#include "ftl_test_env.h"
#include "data_buffer.h"
#include "request_allocation.h"
#include "request_transform.h"
#include "request_format.h"
#include "address_translation.h"
#include "request_schedule.h"

/* data_buffer.h declares `dataBufHashTable` but data_buffer.c defines
 * `dataBufHashTablePtr`. */
extern P_DATA_BUF_HASH_TABLE dataBufHashTablePtr;
/* Defined in request_transform.c but not exported by its header. */
void EvictDataBufEntry(unsigned int originReqSlotTag);

#define ENTRY_COUNT AVAILABLE_DATA_BUFFER_ENTRY_COUNT
#define LAST_ENTRY  (AVAILABLE_DATA_BUFFER_ENTRY_COUNT - 1)

/* The full InitFTL() boot (NAND scan + bad block table) is slow, so boot once
 * and re-initialise the tables these tests touch (request pool, scheduler
 * queues, data buffer) before every test. The eviction tests, which allocate
 * slices, additionally rebuild the address map via reset_address_map(). */
void setUp(void)
{
	static int booted;

	if (!booted) {
		ftl_test_env_init_ftl();
		booted = 1;
	}
	InitReqPool();
	InitDependencyTable();
	InitReqScheduler();
	InitDataBuf();
}
void tearDown(void) {}

/* ----------------------------------------------------------------------- */
/* helpers                                                                 */
/* ----------------------------------------------------------------------- */

static DATA_BUF_ENTRY *buf(unsigned int entry)
{
	return &dataBufMapPtr->dataBuf[entry];
}

static DATA_BUF_HASH_ENTRY *hash_of(unsigned int lsa)
{
	return &dataBufHashTablePtr->dataBufHash[FindDataBufHashTableEntry(lsa)];
}

/* Assign an LSA to a buffer entry and link it into the hash table. */
static void cache_lsa(unsigned int entry, unsigned int lsa)
{
	buf(entry)->logicalSliceAddr = lsa;
	PutToDataBufHashList(entry);
}

/* Take a request slot from the free queue targeting `lsa`. */
static unsigned int new_req_for_lsa(unsigned int lsa)
{
	unsigned int tag = GetFromFreeReqQ();

	reqPoolPtr->reqPool[tag].logicalSliceAddr = lsa;
	reqPoolPtr->reqPool[tag].nvmeCmdSlotTag = 0;
	return tag;
}

/* Walk the LRU list head->tail and check it is a consistent doubly linked
 * list containing exactly ENTRY_COUNT entries. */
static void assert_lru_list_consistent(unsigned int expected_len)
{
	unsigned int entry = dataBufLruList.headEntry;
	unsigned int prev = DATA_BUF_NONE;
	unsigned int len = 0;

	while (entry != DATA_BUF_NONE) {
		TEST_ASSERT_EQUAL_UINT(prev, buf(entry)->prevEntry);
		prev = entry;
		entry = buf(entry)->nextEntry;
		len++;
		TEST_ASSERT_TRUE_MESSAGE(len <= expected_len, "LRU list has a cycle");
	}
	TEST_ASSERT_EQUAL_UINT(expected_len, len);
	TEST_ASSERT_EQUAL_UINT(prev, dataBufLruList.tailEntry);
}

/* Fresh logical->virtual slice map and block/die maps (~1 s: rereads the
 * bad block table from the mocked NAND). */
static void reset_address_map(void)
{
	InitAddressMap();
}

static unsigned int nand_req_total(void)
{
	unsigned int ch, way, total = 0;

	for (ch = 0; ch < USER_CHANNELS; ch++)
		for (way = 0; way < USER_WAYS; way++)
			total += nandReqQ[ch][way].reqCnt;
	return total;
}

/* ----------------------------------------------------------------------- */
/* InitDataBuf                                                             */
/* ----------------------------------------------------------------------- */

static void test_init_builds_full_lru_list_in_index_order(void)
{
	unsigned int i;

	TEST_ASSERT_EQUAL_UINT(0, dataBufLruList.headEntry);
	TEST_ASSERT_EQUAL_UINT(LAST_ENTRY, dataBufLruList.tailEntry);
	assert_lru_list_consistent(ENTRY_COUNT);

	for (i = 0; i < ENTRY_COUNT; i++) {
		TEST_ASSERT_EQUAL_UINT(LSA_NONE, buf(i)->logicalSliceAddr);
		TEST_ASSERT_EQUAL_UINT(DATA_BUF_CLEAN, buf(i)->dirty);
		TEST_ASSERT_EQUAL_UINT(REQ_SLOT_TAG_NONE, buf(i)->blockingReqTail);
		TEST_ASSERT_EQUAL_UINT(DATA_BUF_NONE, buf(i)->hashPrevEntry);
		TEST_ASSERT_EQUAL_UINT(DATA_BUF_NONE, buf(i)->hashNextEntry);
		TEST_ASSERT_EQUAL_UINT(DATA_BUF_NONE, dataBufHashTablePtr->dataBufHash[i].headEntry);
		TEST_ASSERT_EQUAL_UINT(DATA_BUF_NONE, dataBufHashTablePtr->dataBufHash[i].tailEntry);
	}
	for (i = 0; i < AVAILABLE_TEMPORARY_DATA_BUFFER_ENTRY_COUNT; i++)
		TEST_ASSERT_EQUAL_UINT(REQ_SLOT_TAG_NONE,
				       tempDataBufMapPtr->tempDataBuf[i].blockingReqTail);
}

/* ----------------------------------------------------------------------- */
/* AllocateDataBuf / LRU eviction                                          */
/* ----------------------------------------------------------------------- */

static void test_allocate_takes_lru_tail_and_promotes_to_head(void)
{
	unsigned int tail_before = dataBufLruList.tailEntry;
	unsigned int entry;

	TEST_ASSERT_NOT_EQUAL(DATA_BUF_NONE, tail_before);
	entry = AllocateDataBuf();
	TEST_ASSERT_EQUAL_UINT(tail_before, entry);
	TEST_ASSERT_EQUAL_UINT(entry, dataBufLruList.headEntry);
	TEST_ASSERT_EQUAL_UINT(LAST_ENTRY - 1, dataBufLruList.tailEntry);
	TEST_ASSERT_EQUAL_UINT(DATA_BUF_NONE, buf(entry)->prevEntry);
	TEST_ASSERT_EQUAL_UINT(0, buf(entry)->nextEntry);
	TEST_ASSERT_EQUAL_UINT(entry, buf(0)->prevEntry);
	assert_lru_list_consistent(ENTRY_COUNT);
}

static void test_allocating_every_entry_cycles_through_pool_from_tail(void)
{
	unsigned int i;

	for (i = 0; i < ENTRY_COUNT; i++)
		TEST_ASSERT_EQUAL_UINT(LAST_ENTRY - i, AllocateDataBuf());

	/* Full rotation: list order is restored. */
	TEST_ASSERT_EQUAL_UINT(0, dataBufLruList.headEntry);
	TEST_ASSERT_EQUAL_UINT(LAST_ENTRY, dataBufLruList.tailEntry);
	assert_lru_list_consistent(ENTRY_COUNT);

	/* One more wraps around to the original tail again. */
	TEST_ASSERT_EQUAL_UINT(LAST_ENTRY, AllocateDataBuf());
}

static void test_allocate_evicts_least_recently_used_after_hit(void)
{
	unsigned int hit_req, first, second;

	/* Cache LSA 7 in the tail entry, then touch it via a hit so it moves
	 * to the head; the next allocation must not evict it. */
	cache_lsa(LAST_ENTRY, 7);
	hit_req = new_req_for_lsa(7);
	TEST_ASSERT_EQUAL_UINT(LAST_ENTRY, CheckDataBufHit(hit_req));

	first = AllocateDataBuf();
	second = AllocateDataBuf();
	TEST_ASSERT_EQUAL_UINT(LAST_ENTRY - 1, first);
	TEST_ASSERT_EQUAL_UINT(LAST_ENTRY - 2, second);
	TEST_ASSERT_EQUAL_UINT(7, buf(LAST_ENTRY)->logicalSliceAddr);
}

static void test_allocate_unlinks_evicted_entry_from_hash_list(void)
{
	unsigned int entry;

	cache_lsa(LAST_ENTRY, 42);
	TEST_ASSERT_EQUAL_UINT(LAST_ENTRY, hash_of(42)->headEntry);

	entry = AllocateDataBuf();
	TEST_ASSERT_EQUAL_UINT(LAST_ENTRY, entry);
	TEST_ASSERT_EQUAL_UINT(DATA_BUF_NONE, hash_of(42)->headEntry);
	TEST_ASSERT_EQUAL_UINT(DATA_BUF_NONE, hash_of(42)->tailEntry);
	/* The stale LSA stays on the entry until the caller overwrites it. */
	TEST_ASSERT_EQUAL_UINT(42, buf(entry)->logicalSliceAddr);
}

static void test_allocate_from_single_entry_list_keeps_it_as_head_and_tail(void)
{
	unsigned int entry;

	/* Collapse the LRU list to a single entry. */
	dataBufLruList.headEntry = 5;
	dataBufLruList.tailEntry = 5;
	buf(5)->prevEntry = DATA_BUF_NONE;
	buf(5)->nextEntry = DATA_BUF_NONE;

	entry = AllocateDataBuf();
	TEST_ASSERT_EQUAL_UINT(5, entry);
	TEST_ASSERT_EQUAL_UINT(5, dataBufLruList.headEntry);
	TEST_ASSERT_EQUAL_UINT(5, dataBufLruList.tailEntry);
	TEST_ASSERT_EQUAL_UINT(DATA_BUF_NONE, buf(5)->prevEntry);
	TEST_ASSERT_EQUAL_UINT(DATA_BUF_NONE, buf(5)->nextEntry);
}

static void test_allocate_asserts_when_lru_list_is_empty(void)
{
	dataBufLruList.headEntry = DATA_BUF_NONE;
	dataBufLruList.tailEntry = DATA_BUF_NONE;

	FTL_TEST_EXPECT_ASSERT(AllocateDataBuf());
}

/* ----------------------------------------------------------------------- */
/* CheckDataBufHit                                                         */
/* ----------------------------------------------------------------------- */

static void test_lookup_misses_on_empty_pool(void)
{
	unsigned int req = new_req_for_lsa(100);

	TEST_ASSERT_EQUAL_UINT(DATA_BUF_FAIL, CheckDataBufHit(req));
	TEST_ASSERT_EQUAL_UINT(0, dataBufLruList.headEntry);
	TEST_ASSERT_EQUAL_UINT(LAST_ENTRY, dataBufLruList.tailEntry);
}

static void test_lookup_misses_when_hash_bucket_holds_only_other_lsas(void)
{
	/* LSA 3 and LSA 3 + ENTRY_COUNT share a hash bucket. */
	unsigned int req = new_req_for_lsa(3 + ENTRY_COUNT);

	cache_lsa(10, 3);
	TEST_ASSERT_EQUAL_UINT(DATA_BUF_FAIL, CheckDataBufHit(req));
}

static void test_lookup_hit_on_middle_entry_moves_it_to_lru_head(void)
{
	unsigned int req = new_req_for_lsa(55);

	cache_lsa(10, 55);
	TEST_ASSERT_EQUAL_UINT(10, CheckDataBufHit(req));

	TEST_ASSERT_EQUAL_UINT(10, dataBufLruList.headEntry);
	TEST_ASSERT_EQUAL_UINT(DATA_BUF_NONE, buf(10)->prevEntry);
	TEST_ASSERT_EQUAL_UINT(0, buf(10)->nextEntry);
	TEST_ASSERT_EQUAL_UINT(10, buf(0)->prevEntry);
	/* Neighbours are spliced together. */
	TEST_ASSERT_EQUAL_UINT(11, buf(9)->nextEntry);
	TEST_ASSERT_EQUAL_UINT(9, buf(11)->prevEntry);
	TEST_ASSERT_EQUAL_UINT(LAST_ENTRY, dataBufLruList.tailEntry);
	assert_lru_list_consistent(ENTRY_COUNT);
}

static void test_lookup_hit_on_tail_entry_moves_it_to_lru_head(void)
{
	unsigned int req = new_req_for_lsa(55);

	cache_lsa(LAST_ENTRY, 55);
	TEST_ASSERT_EQUAL_UINT(LAST_ENTRY, CheckDataBufHit(req));

	TEST_ASSERT_EQUAL_UINT(LAST_ENTRY, dataBufLruList.headEntry);
	TEST_ASSERT_EQUAL_UINT(LAST_ENTRY - 1, dataBufLruList.tailEntry);
	TEST_ASSERT_EQUAL_UINT(DATA_BUF_NONE, buf(LAST_ENTRY - 1)->nextEntry);
	assert_lru_list_consistent(ENTRY_COUNT);
}

static void test_lookup_hit_on_head_entry_keeps_it_at_head(void)
{
	unsigned int req = new_req_for_lsa(55);

	cache_lsa(0, 55);
	TEST_ASSERT_EQUAL_UINT(0, CheckDataBufHit(req));

	TEST_ASSERT_EQUAL_UINT(0, dataBufLruList.headEntry);
	TEST_ASSERT_EQUAL_UINT(1, buf(0)->nextEntry);
	TEST_ASSERT_EQUAL_UINT(DATA_BUF_NONE, buf(0)->prevEntry);
	TEST_ASSERT_EQUAL_UINT(0, buf(1)->prevEntry);
	TEST_ASSERT_EQUAL_UINT(LAST_ENTRY, dataBufLruList.tailEntry);
	assert_lru_list_consistent(ENTRY_COUNT);
}

static void test_lookup_hit_on_sole_entry_keeps_single_entry_list(void)
{
	unsigned int req = new_req_for_lsa(55);

	dataBufLruList.headEntry = 5;
	dataBufLruList.tailEntry = 5;
	buf(5)->prevEntry = DATA_BUF_NONE;
	buf(5)->nextEntry = DATA_BUF_NONE;
	cache_lsa(5, 55);

	TEST_ASSERT_EQUAL_UINT(5, CheckDataBufHit(req));
	TEST_ASSERT_EQUAL_UINT(5, dataBufLruList.headEntry);
	TEST_ASSERT_EQUAL_UINT(5, dataBufLruList.tailEntry);
	TEST_ASSERT_EQUAL_UINT(DATA_BUF_NONE, buf(5)->prevEntry);
	TEST_ASSERT_EQUAL_UINT(DATA_BUF_NONE, buf(5)->nextEntry);
}

static void test_lookup_walks_hash_chain_to_find_colliding_lsa(void)
{
	unsigned int lsa_a = 4, lsa_b = 4 + ENTRY_COUNT, lsa_c = 4 + 2 * ENTRY_COUNT;
	unsigned int req;

	cache_lsa(20, lsa_a);
	cache_lsa(21, lsa_b);
	cache_lsa(22, lsa_c);

	req = new_req_for_lsa(lsa_c);
	TEST_ASSERT_EQUAL_UINT(22, CheckDataBufHit(req));
	req = new_req_for_lsa(lsa_b);
	TEST_ASSERT_EQUAL_UINT(21, CheckDataBufHit(req));
	req = new_req_for_lsa(lsa_a);
	TEST_ASSERT_EQUAL_UINT(20, CheckDataBufHit(req));
	assert_lru_list_consistent(ENTRY_COUNT);
}

static void test_lookup_does_not_touch_hash_links_on_hit(void)
{
	unsigned int req = new_req_for_lsa(4 + ENTRY_COUNT);

	cache_lsa(20, 4);
	cache_lsa(21, 4 + ENTRY_COUNT);

	TEST_ASSERT_EQUAL_UINT(21, CheckDataBufHit(req));
	TEST_ASSERT_EQUAL_UINT(20, hash_of(4)->headEntry);
	TEST_ASSERT_EQUAL_UINT(21, hash_of(4)->tailEntry);
	TEST_ASSERT_EQUAL_UINT(20, buf(21)->hashPrevEntry);
	TEST_ASSERT_EQUAL_UINT(21, buf(20)->hashNextEntry);
}

/* ----------------------------------------------------------------------- */
/* Hash list maintenance                                                   */
/* ----------------------------------------------------------------------- */

static void test_put_first_entry_becomes_bucket_head_and_tail(void)
{
	cache_lsa(3, 99);

	TEST_ASSERT_EQUAL_UINT(3, hash_of(99)->headEntry);
	TEST_ASSERT_EQUAL_UINT(3, hash_of(99)->tailEntry);
	TEST_ASSERT_EQUAL_UINT(DATA_BUF_NONE, buf(3)->hashPrevEntry);
	TEST_ASSERT_EQUAL_UINT(DATA_BUF_NONE, buf(3)->hashNextEntry);
}

static void test_put_appends_colliding_entries_at_bucket_tail(void)
{
	cache_lsa(3, 99);
	cache_lsa(4, 99 + ENTRY_COUNT);
	cache_lsa(5, 99 + 2 * ENTRY_COUNT);

	TEST_ASSERT_EQUAL_UINT(3, hash_of(99)->headEntry);
	TEST_ASSERT_EQUAL_UINT(5, hash_of(99)->tailEntry);
	TEST_ASSERT_EQUAL_UINT(4, buf(3)->hashNextEntry);
	TEST_ASSERT_EQUAL_UINT(3, buf(4)->hashPrevEntry);
	TEST_ASSERT_EQUAL_UINT(5, buf(4)->hashNextEntry);
	TEST_ASSERT_EQUAL_UINT(4, buf(5)->hashPrevEntry);
	TEST_ASSERT_EQUAL_UINT(DATA_BUF_NONE, buf(5)->hashNextEntry);
}

static void test_put_uses_distinct_buckets_for_non_colliding_lsas(void)
{
	cache_lsa(3, 1);
	cache_lsa(4, 2);

	TEST_ASSERT_EQUAL_UINT(3, hash_of(1)->headEntry);
	TEST_ASSERT_EQUAL_UINT(4, hash_of(2)->headEntry);
	TEST_ASSERT_EQUAL_UINT(DATA_BUF_NONE, buf(3)->hashNextEntry);
	TEST_ASSERT_EQUAL_UINT(DATA_BUF_NONE, buf(4)->hashPrevEntry);
}

static void test_get_ignores_entry_without_lsa(void)
{
	cache_lsa(3, 99);
	TEST_ASSERT_EQUAL_UINT(LSA_NONE, buf(7)->logicalSliceAddr);

	SelectiveGetFromDataBufHashList(7);
	TEST_ASSERT_EQUAL_UINT(3, hash_of(99)->headEntry);
	TEST_ASSERT_EQUAL_UINT(3, hash_of(99)->tailEntry);
}

static void test_get_sole_entry_empties_bucket(void)
{
	cache_lsa(3, 99);
	SelectiveGetFromDataBufHashList(3);

	TEST_ASSERT_EQUAL_UINT(DATA_BUF_NONE, hash_of(99)->headEntry);
	TEST_ASSERT_EQUAL_UINT(DATA_BUF_NONE, hash_of(99)->tailEntry);
}

static void test_get_head_entry_advances_bucket_head(void)
{
	cache_lsa(3, 99);
	cache_lsa(4, 99 + ENTRY_COUNT);
	cache_lsa(5, 99 + 2 * ENTRY_COUNT);

	SelectiveGetFromDataBufHashList(3);
	TEST_ASSERT_EQUAL_UINT(4, hash_of(99)->headEntry);
	TEST_ASSERT_EQUAL_UINT(5, hash_of(99)->tailEntry);
	TEST_ASSERT_EQUAL_UINT(DATA_BUF_NONE, buf(4)->hashPrevEntry);
}

static void test_get_middle_entry_splices_neighbours(void)
{
	cache_lsa(3, 99);
	cache_lsa(4, 99 + ENTRY_COUNT);
	cache_lsa(5, 99 + 2 * ENTRY_COUNT);

	SelectiveGetFromDataBufHashList(4);
	TEST_ASSERT_EQUAL_UINT(3, hash_of(99)->headEntry);
	TEST_ASSERT_EQUAL_UINT(5, hash_of(99)->tailEntry);
	TEST_ASSERT_EQUAL_UINT(5, buf(3)->hashNextEntry);
	TEST_ASSERT_EQUAL_UINT(3, buf(5)->hashPrevEntry);
}

static void test_get_tail_entry_retreats_bucket_tail(void)
{
	cache_lsa(3, 99);
	cache_lsa(4, 99 + ENTRY_COUNT);
	cache_lsa(5, 99 + 2 * ENTRY_COUNT);

	SelectiveGetFromDataBufHashList(5);
	TEST_ASSERT_EQUAL_UINT(3, hash_of(99)->headEntry);
	TEST_ASSERT_EQUAL_UINT(4, hash_of(99)->tailEntry);
	TEST_ASSERT_EQUAL_UINT(DATA_BUF_NONE, buf(4)->hashNextEntry);
}

static void test_removed_entry_is_no_longer_found_by_lookup(void)
{
	unsigned int req = new_req_for_lsa(99 + ENTRY_COUNT);

	cache_lsa(3, 99);
	cache_lsa(4, 99 + ENTRY_COUNT);
	cache_lsa(5, 99 + 2 * ENTRY_COUNT);
	SelectiveGetFromDataBufHashList(4);

	TEST_ASSERT_EQUAL_UINT(DATA_BUF_FAIL, CheckDataBufHit(req));
	req = new_req_for_lsa(99 + 2 * ENTRY_COUNT);
	TEST_ASSERT_EQUAL_UINT(5, CheckDataBufHit(req));
}

/* ----------------------------------------------------------------------- */
/* Blocking request chains                                                 */
/* ----------------------------------------------------------------------- */

static void test_first_blocking_req_becomes_tail_without_links(void)
{
	unsigned int req = GetFromFreeReqQ();

	UpdateDataBufEntryInfoBlockingReq(8, req);
	TEST_ASSERT_EQUAL_UINT(req, buf(8)->blockingReqTail);
	TEST_ASSERT_EQUAL_UINT(REQ_SLOT_TAG_NONE, reqPoolPtr->reqPool[req].prevBlockingReq);
	TEST_ASSERT_EQUAL_UINT(REQ_SLOT_TAG_NONE, reqPoolPtr->reqPool[req].nextBlockingReq);
}

static void test_subsequent_blocking_reqs_are_chained_in_order(void)
{
	unsigned int r1 = GetFromFreeReqQ();
	unsigned int r2 = GetFromFreeReqQ();
	unsigned int r3 = GetFromFreeReqQ();

	UpdateDataBufEntryInfoBlockingReq(8, r1);
	UpdateDataBufEntryInfoBlockingReq(8, r2);
	UpdateDataBufEntryInfoBlockingReq(8, r3);

	TEST_ASSERT_EQUAL_UINT(r3, buf(8)->blockingReqTail);
	TEST_ASSERT_EQUAL_UINT(r2, reqPoolPtr->reqPool[r1].nextBlockingReq);
	TEST_ASSERT_EQUAL_UINT(r1, reqPoolPtr->reqPool[r2].prevBlockingReq);
	TEST_ASSERT_EQUAL_UINT(r3, reqPoolPtr->reqPool[r2].nextBlockingReq);
	TEST_ASSERT_EQUAL_UINT(r2, reqPoolPtr->reqPool[r3].prevBlockingReq);
	TEST_ASSERT_EQUAL_UINT(REQ_SLOT_TAG_NONE, reqPoolPtr->reqPool[r3].nextBlockingReq);
}

static void test_blocking_chains_are_independent_per_entry(void)
{
	unsigned int r1 = GetFromFreeReqQ();
	unsigned int r2 = GetFromFreeReqQ();

	UpdateDataBufEntryInfoBlockingReq(8, r1);
	UpdateDataBufEntryInfoBlockingReq(9, r2);

	TEST_ASSERT_EQUAL_UINT(r1, buf(8)->blockingReqTail);
	TEST_ASSERT_EQUAL_UINT(r2, buf(9)->blockingReqTail);
	TEST_ASSERT_EQUAL_UINT(REQ_SLOT_TAG_NONE, reqPoolPtr->reqPool[r1].nextBlockingReq);
	TEST_ASSERT_EQUAL_UINT(REQ_SLOT_TAG_NONE, reqPoolPtr->reqPool[r2].prevBlockingReq);
}

/* ----------------------------------------------------------------------- */
/* Temporary data buffers                                                  */
/* ----------------------------------------------------------------------- */

static void test_temp_buffer_allocation_is_identity_on_die_number(void)
{
	unsigned int die;

	for (die = 0; die < USER_DIES; die++)
		TEST_ASSERT_EQUAL_UINT(die, AllocateTempDataBuf(die));
}

static void test_temp_buffer_blocking_reqs_are_chained(void)
{
	unsigned int r1 = GetFromFreeReqQ();
	unsigned int r2 = GetFromFreeReqQ();
	unsigned int temp = AllocateTempDataBuf(3);

	UpdateTempDataBufEntryInfoBlockingReq(temp, r1);
	TEST_ASSERT_EQUAL_UINT(r1, tempDataBufMapPtr->tempDataBuf[temp].blockingReqTail);
	TEST_ASSERT_EQUAL_UINT(REQ_SLOT_TAG_NONE, reqPoolPtr->reqPool[r1].prevBlockingReq);

	UpdateTempDataBufEntryInfoBlockingReq(temp, r2);
	TEST_ASSERT_EQUAL_UINT(r2, tempDataBufMapPtr->tempDataBuf[temp].blockingReqTail);
	TEST_ASSERT_EQUAL_UINT(r1, reqPoolPtr->reqPool[r2].prevBlockingReq);
	TEST_ASSERT_EQUAL_UINT(r2, reqPoolPtr->reqPool[r1].nextBlockingReq);
	/* The regular data buffer chains are untouched. */
	TEST_ASSERT_EQUAL_UINT(REQ_SLOT_TAG_NONE, buf(temp)->blockingReqTail);
}

/* ----------------------------------------------------------------------- */
/* Dirty handling: EvictDataBufEntry (request_transform.c)                 */
/* ----------------------------------------------------------------------- */

static void test_evicting_clean_entry_issues_no_nand_write(void)
{
	unsigned int req = new_req_for_lsa(200);
	unsigned int free_before = freeReqQ.reqCnt;

	reset_address_map();
	cache_lsa(12, 77);
	buf(12)->dirty = DATA_BUF_CLEAN;
	reqPoolPtr->reqPool[req].dataBufInfo.entry = 12;

	EvictDataBufEntry(req);

	TEST_ASSERT_EQUAL_UINT(0, nand_req_total());
	TEST_ASSERT_EQUAL_UINT(free_before, freeReqQ.reqCnt);
	TEST_ASSERT_EQUAL_UINT(REQ_SLOT_TAG_NONE, buf(12)->blockingReqTail);
	TEST_ASSERT_EQUAL_UINT(DATA_BUF_CLEAN, buf(12)->dirty);
}

static void test_evicting_dirty_entry_issues_nand_write_and_cleans_it(void)
{
	unsigned int req = new_req_for_lsa(200);
	unsigned int free_before = freeReqQ.reqCnt;
	unsigned int write_req;

	reset_address_map();
	cache_lsa(12, 77);
	buf(12)->dirty = DATA_BUF_DIRTY;
	reqPoolPtr->reqPool[req].dataBufInfo.entry = 12;
	reqPoolPtr->reqPool[req].nvmeCmdSlotTag = 9;

	EvictDataBufEntry(req);

	TEST_ASSERT_EQUAL_UINT(DATA_BUF_CLEAN, buf(12)->dirty);
	TEST_ASSERT_EQUAL_UINT(free_before - 1, freeReqQ.reqCnt);
	TEST_ASSERT_EQUAL_UINT(1, nand_req_total());

	write_req = buf(12)->blockingReqTail;
	TEST_ASSERT_NOT_EQUAL(REQ_SLOT_TAG_NONE, write_req);
	TEST_ASSERT_EQUAL_UINT(REQ_TYPE_NAND, reqPoolPtr->reqPool[write_req].reqType);
	TEST_ASSERT_EQUAL_UINT(REQ_CODE_WRITE, reqPoolPtr->reqPool[write_req].reqCode);
	TEST_ASSERT_EQUAL_UINT(9, reqPoolPtr->reqPool[write_req].nvmeCmdSlotTag);
	TEST_ASSERT_EQUAL_UINT(77, reqPoolPtr->reqPool[write_req].logicalSliceAddr);
	TEST_ASSERT_EQUAL_UINT(12, reqPoolPtr->reqPool[write_req].dataBufInfo.entry);
	TEST_ASSERT_EQUAL_UINT(REQ_OPT_DATA_BUF_ENTRY, reqPoolPtr->reqPool[write_req].reqOpt.dataBufFormat);
	TEST_ASSERT_EQUAL_UINT(REQ_OPT_NAND_ADDR_VSA, reqPoolPtr->reqPool[write_req].reqOpt.nandAddr);
	TEST_ASSERT_NOT_EQUAL(VSA_FAIL, reqPoolPtr->reqPool[write_req].nandInfo.virtualSliceAddr);
	/* The write is now the mapped location of LSA 77. */
	TEST_ASSERT_EQUAL_UINT(reqPoolPtr->reqPool[write_req].nandInfo.virtualSliceAddr,
			       logicalSliceMapPtr->logicalSlice[77].virtualSliceAddr);
}

static void test_dirty_eviction_write_is_blocked_behind_pending_req_on_entry(void)
{
	unsigned int req = new_req_for_lsa(200);
	unsigned int pending = GetFromFreeReqQ();
	unsigned int write_req;

	reset_address_map();
	cache_lsa(12, 77);
	buf(12)->dirty = DATA_BUF_DIRTY;
	reqPoolPtr->reqPool[req].dataBufInfo.entry = 12;
	UpdateDataBufEntryInfoBlockingReq(12, pending);

	EvictDataBufEntry(req);

	write_req = buf(12)->blockingReqTail;
	TEST_ASSERT_NOT_EQUAL(pending, write_req);
	TEST_ASSERT_EQUAL_UINT(pending, reqPoolPtr->reqPool[write_req].prevBlockingReq);
	TEST_ASSERT_EQUAL_UINT(write_req, reqPoolPtr->reqPool[pending].nextBlockingReq);
	TEST_ASSERT_EQUAL_UINT(0, nand_req_total());
	TEST_ASSERT_EQUAL_UINT(1, blockedByBufDepReqQ.reqCnt);
	TEST_ASSERT_EQUAL_UINT(write_req, blockedByBufDepReqQ.headReq);
	TEST_ASSERT_EQUAL_UINT(DATA_BUF_CLEAN, buf(12)->dirty);
}

static void test_completing_pending_req_releases_blocked_eviction_write_to_nand(void)
{
	unsigned int req = new_req_for_lsa(200);
	unsigned int pending = GetFromFreeReqQ();
	unsigned int write_req;

	reset_address_map();
	cache_lsa(12, 77);
	buf(12)->dirty = DATA_BUF_DIRTY;
	reqPoolPtr->reqPool[req].dataBufInfo.entry = 12;
	reqPoolPtr->reqPool[pending].reqOpt.dataBufFormat = REQ_OPT_DATA_BUF_ENTRY;
	reqPoolPtr->reqPool[pending].dataBufInfo.entry = 12;
	UpdateDataBufEntryInfoBlockingReq(12, pending);
	EvictDataBufEntry(req);
	write_req = buf(12)->blockingReqTail;
	TEST_ASSERT_EQUAL_UINT(0, nand_req_total());

	ReleaseBlockedByBufDepReq(pending);

	TEST_ASSERT_EQUAL_UINT(0, blockedByBufDepReqQ.reqCnt);
	TEST_ASSERT_EQUAL_UINT(1, nand_req_total());
	TEST_ASSERT_EQUAL_UINT(REQ_QUEUE_TYPE_NAND, reqPoolPtr->reqPool[write_req].reqQueueType);
	TEST_ASSERT_EQUAL_UINT(REQ_SLOT_TAG_NONE, reqPoolPtr->reqPool[write_req].prevBlockingReq);
	TEST_ASSERT_EQUAL_UINT(REQ_SLOT_TAG_NONE, reqPoolPtr->reqPool[pending].nextBlockingReq);
	/* The write is still the newest request on the entry. */
	TEST_ASSERT_EQUAL_UINT(write_req, buf(12)->blockingReqTail);
}

int main(void)
{
	UNITY_BEGIN();
	RUN_TEST(test_init_builds_full_lru_list_in_index_order);

	RUN_TEST(test_allocate_takes_lru_tail_and_promotes_to_head);
	RUN_TEST(test_allocating_every_entry_cycles_through_pool_from_tail);
	RUN_TEST(test_allocate_evicts_least_recently_used_after_hit);
	RUN_TEST(test_allocate_unlinks_evicted_entry_from_hash_list);
	RUN_TEST(test_allocate_from_single_entry_list_keeps_it_as_head_and_tail);
	RUN_TEST(test_allocate_asserts_when_lru_list_is_empty);

	RUN_TEST(test_lookup_misses_on_empty_pool);
	RUN_TEST(test_lookup_misses_when_hash_bucket_holds_only_other_lsas);
	RUN_TEST(test_lookup_hit_on_middle_entry_moves_it_to_lru_head);
	RUN_TEST(test_lookup_hit_on_tail_entry_moves_it_to_lru_head);
	RUN_TEST(test_lookup_hit_on_head_entry_keeps_it_at_head);
	RUN_TEST(test_lookup_hit_on_sole_entry_keeps_single_entry_list);
	RUN_TEST(test_lookup_walks_hash_chain_to_find_colliding_lsa);
	RUN_TEST(test_lookup_does_not_touch_hash_links_on_hit);

	RUN_TEST(test_put_first_entry_becomes_bucket_head_and_tail);
	RUN_TEST(test_put_appends_colliding_entries_at_bucket_tail);
	RUN_TEST(test_put_uses_distinct_buckets_for_non_colliding_lsas);
	RUN_TEST(test_get_ignores_entry_without_lsa);
	RUN_TEST(test_get_sole_entry_empties_bucket);
	RUN_TEST(test_get_head_entry_advances_bucket_head);
	RUN_TEST(test_get_middle_entry_splices_neighbours);
	RUN_TEST(test_get_tail_entry_retreats_bucket_tail);
	RUN_TEST(test_removed_entry_is_no_longer_found_by_lookup);

	RUN_TEST(test_first_blocking_req_becomes_tail_without_links);
	RUN_TEST(test_subsequent_blocking_reqs_are_chained_in_order);
	RUN_TEST(test_blocking_chains_are_independent_per_entry);

	RUN_TEST(test_temp_buffer_allocation_is_identity_on_die_number);
	RUN_TEST(test_temp_buffer_blocking_reqs_are_chained);

	RUN_TEST(test_evicting_clean_entry_issues_no_nand_write);
	RUN_TEST(test_evicting_dirty_entry_issues_nand_write_and_cleans_it);
	RUN_TEST(test_dirty_eviction_write_is_blocked_behind_pending_req_on_entry);
	RUN_TEST(test_completing_pending_req_releases_blocked_eviction_write_to_nand);
	return UNITY_END();
}
