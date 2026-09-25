/* Unit tests for data_buffer.c. */
#include "unity.h"

#include "fw_test.h"
#include "memory_map.h"

/* data_buffer.h declares this as dataBufHashTable; the definition is *Ptr. */
extern P_DATA_BUF_HASH_TABLE dataBufHashTablePtr;

#define LAST_ENTRY (AVAILABLE_DATA_BUFFER_ENTRY_COUNT - 1)

void setUp(void)
{
	fw_test_reset();
	fw_test_init_ftl();
}

void tearDown(void) {}

static DATA_BUF_ENTRY *buf(unsigned int entry)
{
	return &dataBufMapPtr->dataBuf[entry];
}

static unsigned int req_for_lsa(unsigned int lsa)
{
	unsigned int reqSlotTag = GetFromFreeReqQ();

	reqPoolPtr->reqPool[reqSlotTag].logicalSliceAddr = lsa;
	return reqSlotTag;
}

static void insert(unsigned int entry, unsigned int lsa)
{
	buf(entry)->logicalSliceAddr = lsa;
	PutToDataBufHashList(entry);
}

/* Walks the LRU list head to tail and checks it is consistent. */
static unsigned int lru_length(void)
{
	unsigned int entry = dataBufLruList.headEntry;
	unsigned int prev = DATA_BUF_NONE;
	unsigned int length = 0;

	while (entry != DATA_BUF_NONE) {
		TEST_ASSERT_EQUAL_UINT(prev, buf(entry)->prevEntry);
		prev = entry;
		entry = buf(entry)->nextEntry;
		length++;
	}
	TEST_ASSERT_EQUAL_UINT(prev, dataBufLruList.tailEntry);
	return length;
}

static void test_smoke_lookup_misses_on_empty_buffer(void)
{
	unsigned int reqSlotTag = req_for_lsa(42);

	TEST_ASSERT_EQUAL_HEX32(DATA_BUF_FAIL, CheckDataBufHit(reqSlotTag));
}

static void test_smoke_allocated_entry_hits_after_hash_insert(void)
{
	unsigned int reqSlotTag = req_for_lsa(42);
	unsigned int bufEntry = AllocateDataBuf();

	TEST_ASSERT_NOT_EQUAL(DATA_BUF_FAIL, bufEntry);
	insert(bufEntry, 42);

	TEST_ASSERT_EQUAL_UINT(bufEntry, CheckDataBufHit(reqSlotTag));
	TEST_ASSERT_EQUAL_UINT(bufEntry, dataBufLruList.headEntry);
}

static void test_init_builds_full_lru_list_with_empty_hash_table(void)
{
	unsigned int entry;

	TEST_ASSERT_EQUAL_UINT(AVAILABLE_DATA_BUFFER_ENTRY_COUNT, lru_length());
	TEST_ASSERT_EQUAL_UINT(0, dataBufLruList.headEntry);
	TEST_ASSERT_EQUAL_UINT(LAST_ENTRY, dataBufLruList.tailEntry);
	for (entry = 0; entry < AVAILABLE_DATA_BUFFER_ENTRY_COUNT; entry++) {
		TEST_ASSERT_EQUAL_HEX32(LSA_NONE, buf(entry)->logicalSliceAddr);
		TEST_ASSERT_EQUAL_UINT(DATA_BUF_CLEAN, buf(entry)->dirty);
		TEST_ASSERT_EQUAL_UINT(REQ_SLOT_TAG_NONE, buf(entry)->blockingReqTail);
		TEST_ASSERT_EQUAL_UINT(DATA_BUF_NONE, dataBufHashTablePtr->dataBufHash[entry].headEntry);
	}
	TEST_ASSERT_EQUAL_UINT(REQ_SLOT_TAG_NONE, tempDataBufMapPtr->tempDataBuf[0].blockingReqTail);
}

static void test_allocate_evicts_lru_tail_and_moves_it_to_head(void)
{
	unsigned int first = AllocateDataBuf();
	unsigned int second = AllocateDataBuf();

	TEST_ASSERT_EQUAL_UINT(LAST_ENTRY, first);
	TEST_ASSERT_EQUAL_UINT(LAST_ENTRY - 1, second);
	TEST_ASSERT_EQUAL_UINT(second, dataBufLruList.headEntry);
	TEST_ASSERT_EQUAL_UINT(first, buf(second)->nextEntry);
	TEST_ASSERT_EQUAL_UINT(LAST_ENTRY - 2, dataBufLruList.tailEntry);
	TEST_ASSERT_EQUAL_UINT(AVAILABLE_DATA_BUFFER_ENTRY_COUNT, lru_length());
}

static void test_allocate_cycles_through_every_entry_before_reusing_one(void)
{
	unsigned int i;

	for (i = 0; i < AVAILABLE_DATA_BUFFER_ENTRY_COUNT; i++)
		TEST_ASSERT_EQUAL_UINT(LAST_ENTRY - i, AllocateDataBuf());

	TEST_ASSERT_EQUAL_UINT(LAST_ENTRY, AllocateDataBuf());
	TEST_ASSERT_EQUAL_UINT(AVAILABLE_DATA_BUFFER_ENTRY_COUNT, lru_length());
}

static void test_allocate_removes_evicted_entry_from_hash_list(void)
{
	unsigned int reqSlotTag = req_for_lsa(7);
	unsigned int entry = AllocateDataBuf();
	unsigned int i;

	insert(entry, 7);
	TEST_ASSERT_EQUAL_UINT(entry, CheckDataBufHit(reqSlotTag));

	for (i = 0; i < AVAILABLE_DATA_BUFFER_ENTRY_COUNT; i++)
		AllocateDataBuf();

	TEST_ASSERT_EQUAL_HEX32(DATA_BUF_FAIL, CheckDataBufHit(reqSlotTag));
}

static void test_allocate_asserts_when_lru_list_is_empty(void)
{
	dataBufLruList.tailEntry = DATA_BUF_NONE;

	FW_EXPECT_ASSERT(AllocateDataBuf());
}

static void test_allocate_single_entry_list_keeps_it_as_head_and_tail(void)
{
	dataBufLruList.headEntry = 3;
	dataBufLruList.tailEntry = 3;
	buf(3)->prevEntry = DATA_BUF_NONE;
	buf(3)->nextEntry = DATA_BUF_NONE;

	TEST_ASSERT_EQUAL_UINT(3, AllocateDataBuf());
	TEST_ASSERT_EQUAL_UINT(3, dataBufLruList.headEntry);
	TEST_ASSERT_EQUAL_UINT(3, dataBufLruList.tailEntry);
}

static void test_hit_on_middle_entry_moves_it_to_head(void)
{
	unsigned int reqSlotTag = req_for_lsa(11);

	insert(5, 11);

	TEST_ASSERT_EQUAL_UINT(5, CheckDataBufHit(reqSlotTag));
	TEST_ASSERT_EQUAL_UINT(5, dataBufLruList.headEntry);
	TEST_ASSERT_EQUAL_UINT(0, buf(5)->nextEntry);
	TEST_ASSERT_EQUAL_UINT(6, buf(4)->nextEntry);
	TEST_ASSERT_EQUAL_UINT(4, buf(6)->prevEntry);
	TEST_ASSERT_EQUAL_UINT(AVAILABLE_DATA_BUFFER_ENTRY_COUNT, lru_length());
}

static void test_hit_on_tail_entry_moves_it_to_head(void)
{
	unsigned int reqSlotTag = req_for_lsa(11);

	insert(LAST_ENTRY, 11);

	TEST_ASSERT_EQUAL_UINT(LAST_ENTRY, CheckDataBufHit(reqSlotTag));
	TEST_ASSERT_EQUAL_UINT(LAST_ENTRY, dataBufLruList.headEntry);
	TEST_ASSERT_EQUAL_UINT(LAST_ENTRY - 1, dataBufLruList.tailEntry);
	TEST_ASSERT_EQUAL_UINT(AVAILABLE_DATA_BUFFER_ENTRY_COUNT, lru_length());
}

static void test_hit_on_head_entry_keeps_list_intact(void)
{
	unsigned int reqSlotTag = req_for_lsa(11);

	insert(0, 11);

	TEST_ASSERT_EQUAL_UINT(0, CheckDataBufHit(reqSlotTag));
	TEST_ASSERT_EQUAL_UINT(0, dataBufLruList.headEntry);
	TEST_ASSERT_EQUAL_UINT(1, buf(0)->nextEntry);
	TEST_ASSERT_EQUAL_UINT(AVAILABLE_DATA_BUFFER_ENTRY_COUNT, lru_length());
}

static void test_hit_on_sole_lru_entry_keeps_it_as_head_and_tail(void)
{
	unsigned int reqSlotTag = req_for_lsa(11);

	dataBufLruList.headEntry = 2;
	dataBufLruList.tailEntry = 2;
	buf(2)->prevEntry = DATA_BUF_NONE;
	buf(2)->nextEntry = DATA_BUF_NONE;
	insert(2, 11);

	TEST_ASSERT_EQUAL_UINT(2, CheckDataBufHit(reqSlotTag));
	TEST_ASSERT_EQUAL_UINT(2, dataBufLruList.headEntry);
	TEST_ASSERT_EQUAL_UINT(2, dataBufLruList.tailEntry);
}

static void test_colliding_slices_share_a_hash_bucket(void)
{
	const unsigned int lsaA = 3;
	const unsigned int lsaB = 3 + AVAILABLE_DATA_BUFFER_ENTRY_COUNT;
	const unsigned int lsaC = 3 + 2 * AVAILABLE_DATA_BUFFER_ENTRY_COUNT;
	unsigned int bucket = FindDataBufHashTableEntry(lsaA);

	insert(1, lsaA);
	insert(2, lsaB);
	insert(3, lsaC);

	TEST_ASSERT_EQUAL_UINT(1, dataBufHashTablePtr->dataBufHash[bucket].headEntry);
	TEST_ASSERT_EQUAL_UINT(3, dataBufHashTablePtr->dataBufHash[bucket].tailEntry);
	TEST_ASSERT_EQUAL_UINT(2, buf(1)->hashNextEntry);
	TEST_ASSERT_EQUAL_UINT(1, buf(2)->hashPrevEntry);

	TEST_ASSERT_EQUAL_UINT(2, CheckDataBufHit(req_for_lsa(lsaB)));
	TEST_ASSERT_EQUAL_UINT(3, CheckDataBufHit(req_for_lsa(lsaC)));
	TEST_ASSERT_EQUAL_UINT(1, CheckDataBufHit(req_for_lsa(lsaA)));
	TEST_ASSERT_EQUAL_HEX32(DATA_BUF_FAIL,
			CheckDataBufHit(req_for_lsa(lsaC + AVAILABLE_DATA_BUFFER_ENTRY_COUNT)));
}

static void test_hash_removal_handles_middle_head_and_tail(void)
{
	const unsigned int lsaA = 3;
	const unsigned int lsaB = 3 + AVAILABLE_DATA_BUFFER_ENTRY_COUNT;
	const unsigned int lsaC = 3 + 2 * AVAILABLE_DATA_BUFFER_ENTRY_COUNT;
	unsigned int bucket = FindDataBufHashTableEntry(lsaA);

	insert(1, lsaA);
	insert(2, lsaB);
	insert(3, lsaC);

	SelectiveGetFromDataBufHashList(2);
	TEST_ASSERT_EQUAL_UINT(3, buf(1)->hashNextEntry);
	TEST_ASSERT_EQUAL_UINT(1, buf(3)->hashPrevEntry);

	SelectiveGetFromDataBufHashList(3);
	TEST_ASSERT_EQUAL_UINT(1, dataBufHashTablePtr->dataBufHash[bucket].tailEntry);
	TEST_ASSERT_EQUAL_UINT(DATA_BUF_NONE, buf(1)->hashNextEntry);

	insert(4, lsaB);
	SelectiveGetFromDataBufHashList(1);
	TEST_ASSERT_EQUAL_UINT(4, dataBufHashTablePtr->dataBufHash[bucket].headEntry);
	TEST_ASSERT_EQUAL_UINT(DATA_BUF_NONE, buf(4)->hashPrevEntry);

	SelectiveGetFromDataBufHashList(4);
	TEST_ASSERT_EQUAL_UINT(DATA_BUF_NONE, dataBufHashTablePtr->dataBufHash[bucket].headEntry);
	TEST_ASSERT_EQUAL_UINT(DATA_BUF_NONE, dataBufHashTablePtr->dataBufHash[bucket].tailEntry);
}

static void test_hash_removal_of_unmapped_entry_is_a_no_op(void)
{
	insert(1, 3);

	SelectiveGetFromDataBufHashList(2);

	TEST_ASSERT_EQUAL_UINT(1, dataBufHashTablePtr->dataBufHash[FindDataBufHashTableEntry(3)].headEntry);
}

static void test_blocking_requests_chain_in_arrival_order(void)
{
	unsigned int first = GetFromFreeReqQ();
	unsigned int second = GetFromFreeReqQ();

	UpdateDataBufEntryInfoBlockingReq(4, first);
	TEST_ASSERT_EQUAL_UINT(first, buf(4)->blockingReqTail);
	TEST_ASSERT_EQUAL_UINT(REQ_SLOT_TAG_NONE, reqPoolPtr->reqPool[first].prevBlockingReq);

	UpdateDataBufEntryInfoBlockingReq(4, second);
	TEST_ASSERT_EQUAL_UINT(second, buf(4)->blockingReqTail);
	TEST_ASSERT_EQUAL_UINT(first, reqPoolPtr->reqPool[second].prevBlockingReq);
	TEST_ASSERT_EQUAL_UINT(second, reqPoolPtr->reqPool[first].nextBlockingReq);
}

static void test_temp_buffer_blocking_requests_chain_per_die(void)
{
	unsigned int first = GetFromFreeReqQ();
	unsigned int second = GetFromFreeReqQ();
	unsigned int entry = AllocateTempDataBuf(USER_DIES - 1);

	TEST_ASSERT_EQUAL_UINT(USER_DIES - 1, entry);

	UpdateTempDataBufEntryInfoBlockingReq(entry, first);
	TEST_ASSERT_EQUAL_UINT(first, tempDataBufMapPtr->tempDataBuf[entry].blockingReqTail);

	UpdateTempDataBufEntryInfoBlockingReq(entry, second);
	TEST_ASSERT_EQUAL_UINT(second, tempDataBufMapPtr->tempDataBuf[entry].blockingReqTail);
	TEST_ASSERT_EQUAL_UINT(first, reqPoolPtr->reqPool[second].prevBlockingReq);
	TEST_ASSERT_EQUAL_UINT(second, reqPoolPtr->reqPool[first].nextBlockingReq);
}

int main(void)
{
	UNITY_BEGIN();
	RUN_TEST(test_smoke_lookup_misses_on_empty_buffer);
	RUN_TEST(test_smoke_allocated_entry_hits_after_hash_insert);
	RUN_TEST(test_init_builds_full_lru_list_with_empty_hash_table);
	RUN_TEST(test_allocate_evicts_lru_tail_and_moves_it_to_head);
	RUN_TEST(test_allocate_cycles_through_every_entry_before_reusing_one);
	RUN_TEST(test_allocate_removes_evicted_entry_from_hash_list);
	RUN_TEST(test_allocate_asserts_when_lru_list_is_empty);
	RUN_TEST(test_allocate_single_entry_list_keeps_it_as_head_and_tail);
	RUN_TEST(test_hit_on_middle_entry_moves_it_to_head);
	RUN_TEST(test_hit_on_tail_entry_moves_it_to_head);
	RUN_TEST(test_hit_on_head_entry_keeps_list_intact);
	RUN_TEST(test_hit_on_sole_lru_entry_keeps_it_as_head_and_tail);
	RUN_TEST(test_colliding_slices_share_a_hash_bucket);
	RUN_TEST(test_hash_removal_handles_middle_head_and_tail);
	RUN_TEST(test_hash_removal_of_unmapped_entry_is_a_no_op);
	RUN_TEST(test_blocking_requests_chain_in_arrival_order);
	RUN_TEST(test_temp_buffer_blocking_requests_chain_per_die);
	return UNITY_END();
}
