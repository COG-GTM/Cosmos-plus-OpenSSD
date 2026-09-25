#include "unity.h"

#include "ftl_test_env.h"

#define CMD_SLOT 3u
#define ENTRIES AVAILABLE_DATA_BUFFER_ENTRY_COUNT

void setUp(void)
{
	ftl_test_env_init();
}

void tearDown(void)
{
}

/* Push one full-slice NVMe I/O through the slice layer and let it complete. */
static void IssueIo(unsigned int lsa, unsigned int cmdCode)
{
	ReqTransNvmeToSlice(CMD_SLOT, lsa * NVME_BLOCKS_PER_SLICE, NVME_BLOCKS_PER_SLICE - 1, cmdCode);
	ReqTransSliceToLowLevel();
	ftl_test_drain();
}

/* Side-effect-free lookup: walk the hash chain the way CheckDataBufHit does. */
static unsigned int FindEntry(unsigned int lsa)
{
	unsigned int entry = dataBufHashTablePtr->dataBufHash[FindDataBufHashTableEntry(lsa)].headEntry;
	while (entry != DATA_BUF_NONE)
	{
		if (dataBufMapPtr->dataBuf[entry].logicalSliceAddr == lsa)
			return entry;
		entry = dataBufMapPtr->dataBuf[entry].hashNextEntry;
	}
	return DATA_BUF_FAIL;
}

static unsigned int LruTailLsa(void)
{
	return dataBufMapPtr->dataBuf[dataBufLruList.tailEntry].logicalSliceAddr;
}

static unsigned int TotalPrograms(void)
{
	unsigned int ch, way, n = 0;
	for (ch = 0; ch < USER_CHANNELS; ch++)
		for (way = 0; way < USER_WAYS; way++)
			n += fake_nand_stats(ch, way).programs;
	return n;
}

static unsigned int TotalReadTransfers(void)
{
	unsigned int ch, way, n = 0;
	for (ch = 0; ch < USER_CHANNELS; ch++)
		for (way = 0; way < USER_WAYS; way++)
			n += fake_nand_stats(ch, way).readTransfers;
	return n;
}

/* ---- Initial state ------------------------------------------------------ */

static void test_init_links_every_entry_into_lru_and_no_hash_chains(void)
{
	unsigned int i;

	TEST_ASSERT_EQUAL_INT(ENTRIES, ftl_test_count_lru());
	TEST_ASSERT_EQUAL_UINT32(0, dataBufLruList.headEntry);
	TEST_ASSERT_EQUAL_UINT32(ENTRIES - 1, dataBufLruList.tailEntry);
	for (i = 0; i < ENTRIES; i++)
	{
		TEST_ASSERT_EQUAL_HEX32(LSA_NONE, dataBufMapPtr->dataBuf[i].logicalSliceAddr);
		TEST_ASSERT_EQUAL_UINT32(DATA_BUF_CLEAN, dataBufMapPtr->dataBuf[i].dirty);
		TEST_ASSERT_EQUAL_UINT32(DATA_BUF_NONE, dataBufHashTablePtr->dataBufHash[i].headEntry);
	}
	TEST_ASSERT_EQUAL_UINT32(REQ_SLOT_TAG_NONE, dataBufMapPtr->dataBuf[0].blockingReqTail);
}

/* ---- Hit / miss ---------------------------------------------------------- */

static void test_lookup_misses_for_unseen_lsa(void)
{
	unsigned int tag = GetFromFreeReqQ();
	reqPoolPtr->reqPool[tag].logicalSliceAddr = 1234;
	TEST_ASSERT_EQUAL_UINT32(DATA_BUF_FAIL, CheckDataBufHit(tag));
	PutToFreeReqQ(tag);
}

static void test_miss_allocates_lru_tail_and_hit_finds_same_entry(void)
{
	unsigned int entry, tag;

	IssueIo(100, IO_NVM_WRITE);

	entry = FindEntry(100);
	TEST_ASSERT_EQUAL_UINT32(ENTRIES - 1, entry); /* the pre-init LRU tail */
	TEST_ASSERT_EQUAL_UINT32(entry, dataBufLruList.headEntry);
	TEST_ASSERT_EQUAL_UINT32(DATA_BUF_DIRTY, dataBufMapPtr->dataBuf[entry].dirty);

	tag = GetFromFreeReqQ();
	reqPoolPtr->reqPool[tag].logicalSliceAddr = 100;
	TEST_ASSERT_EQUAL_UINT32(entry, CheckDataBufHit(tag));
	PutToFreeReqQ(tag);
}

static void test_read_hit_is_served_from_buffer_without_nand_access(void)
{
	unsigned int reads, txBefore;

	IssueIo(7, IO_NVM_WRITE);
	reads = TotalReadTransfers();
	txBefore = fake_dma_count_of(HOST_DMA_AUTO_TYPE, HOST_DMA_TX_DIRECTION);

	IssueIo(7, IO_NVM_READ);

	TEST_ASSERT_EQUAL_UINT32(reads, TotalReadTransfers());
	TEST_ASSERT_EQUAL_UINT32(txBefore + NVME_BLOCKS_PER_SLICE,
							 fake_dma_count_of(HOST_DMA_AUTO_TYPE, HOST_DMA_TX_DIRECTION));
	TEST_ASSERT_EQUAL_INT(ENTRIES, ftl_test_count_lru());
}

static void test_read_miss_of_flashed_lsa_fetches_from_nand(void)
{
	unsigned int reads;

	ftl_test_write_slice(42, 0xA5); /* bypasses the buffer: data only on NAND */
	TEST_ASSERT_EQUAL_UINT32(DATA_BUF_FAIL, FindEntry(42));
	reads = TotalReadTransfers();

	IssueIo(42, IO_NVM_READ);

	TEST_ASSERT_EQUAL_UINT32(reads + 1, TotalReadTransfers());
	TEST_ASSERT_NOT_EQUAL(DATA_BUF_FAIL, FindEntry(42));
	TEST_ASSERT_EQUAL_UINT32(DATA_BUF_CLEAN, dataBufMapPtr->dataBuf[FindEntry(42)].dirty);
}

static void test_read_miss_of_never_written_lsa_skips_nand(void)
{
	unsigned int reads = TotalReadTransfers();
	IssueIo(4242, IO_NVM_READ);
	TEST_ASSERT_EQUAL_UINT32(reads, TotalReadTransfers());
	TEST_ASSERT_NOT_EQUAL(DATA_BUF_FAIL, FindEntry(4242));
}

static void test_colliding_lsas_share_hash_chain_and_both_hit(void)
{
	unsigned int a = 5, b = 5 + ENTRIES, bucket = FindDataBufHashTableEntry(a);

	TEST_ASSERT_EQUAL_UINT32(bucket, FindDataBufHashTableEntry(b));
	IssueIo(a, IO_NVM_WRITE);
	IssueIo(b, IO_NVM_WRITE);

	TEST_ASSERT_EQUAL_UINT32(FindEntry(a), dataBufHashTablePtr->dataBufHash[bucket].headEntry);
	TEST_ASSERT_EQUAL_UINT32(FindEntry(b), dataBufHashTablePtr->dataBufHash[bucket].tailEntry);
	TEST_ASSERT_EQUAL_UINT32(FindEntry(b), dataBufMapPtr->dataBuf[FindEntry(a)].hashNextEntry);
	TEST_ASSERT_NOT_EQUAL(FindEntry(a), FindEntry(b));
}

/* ---- LRU ordering --------------------------------------------------------- */

static void test_hit_moves_entry_to_lru_head(void)
{
	IssueIo(10, IO_NVM_WRITE);
	IssueIo(11, IO_NVM_WRITE);
	IssueIo(12, IO_NVM_WRITE);
	TEST_ASSERT_EQUAL_UINT32(FindEntry(12), dataBufLruList.headEntry);

	IssueIo(10, IO_NVM_READ);

	TEST_ASSERT_EQUAL_UINT32(FindEntry(10), dataBufLruList.headEntry);
	TEST_ASSERT_EQUAL_UINT32(FindEntry(12), dataBufMapPtr->dataBuf[FindEntry(10)].nextEntry);
	TEST_ASSERT_EQUAL_INT(ENTRIES, ftl_test_count_lru());
}

static void test_eviction_takes_least_recently_used_entry(void)
{
	unsigned int lsa;

	for (lsa = 0; lsa < ENTRIES; lsa++)
		IssueIo(lsa, IO_NVM_WRITE);
	TEST_ASSERT_EQUAL_UINT32(0, LruTailLsa());

	IssueIo(1, IO_NVM_READ); /* touch LSA 1 so it is not the victim */
	IssueIo(ENTRIES, IO_NVM_WRITE);

	TEST_ASSERT_EQUAL_UINT32(DATA_BUF_FAIL, FindEntry(0));
	TEST_ASSERT_NOT_EQUAL(DATA_BUF_FAIL, FindEntry(1));
	TEST_ASSERT_NOT_EQUAL(DATA_BUF_FAIL, FindEntry(ENTRIES));
	TEST_ASSERT_EQUAL_UINT32(2, LruTailLsa());
	TEST_ASSERT_EQUAL_INT(ENTRIES, ftl_test_count_lru());
}

static void test_eviction_leaves_hash_chains_consistent(void)
{
	unsigned int lsa, bucket;

	for (lsa = 0; lsa < ENTRIES; lsa++)
		IssueIo(lsa, IO_NVM_WRITE);
	IssueIo(ENTRIES, IO_NVM_WRITE); /* LSA 0 and LSA ENTRIES share a bucket */

	bucket = FindDataBufHashTableEntry(0);
	TEST_ASSERT_EQUAL_UINT32(FindEntry(ENTRIES), dataBufHashTablePtr->dataBufHash[bucket].headEntry);
	TEST_ASSERT_EQUAL_UINT32(FindEntry(ENTRIES), dataBufHashTablePtr->dataBufHash[bucket].tailEntry);
	TEST_ASSERT_EQUAL_UINT32(DATA_BUF_NONE, dataBufMapPtr->dataBuf[FindEntry(ENTRIES)].hashNextEntry);
	for (lsa = 1; lsa < ENTRIES; lsa++)
		TEST_ASSERT_NOT_EQUAL(DATA_BUF_FAIL, FindEntry(lsa));
}

/* ---- Dirty eviction ------------------------------------------------------ */

static void test_dirty_eviction_writes_victim_back_to_nand(void)
{
	unsigned int lsa, programs;

	for (lsa = 0; lsa < ENTRIES; lsa++)
		IssueIo(lsa, IO_NVM_WRITE);
	TEST_ASSERT_EQUAL_HEX32(VSA_FAIL, AddrTransRead(0)); /* still only in buffer */
	programs = TotalPrograms();

	IssueIo(ENTRIES, IO_NVM_WRITE);

	TEST_ASSERT_EQUAL_UINT32(programs + 1, TotalPrograms());
	TEST_ASSERT_NOT_EQUAL(VSA_FAIL, AddrTransRead(0));
	TEST_ASSERT_EQUAL_HEX32(VSA_FAIL, AddrTransRead(1));
	TEST_ASSERT_EQUAL_UINT32(DATA_BUF_DIRTY, dataBufMapPtr->dataBuf[FindEntry(ENTRIES)].dirty);
}

static void test_clean_eviction_does_not_touch_nand(void)
{
	unsigned int lsa, programs;

	for (lsa = 0; lsa < ENTRIES; lsa++)
		IssueIo(1000 + lsa, IO_NVM_READ); /* unwritten LSAs: clean entries */
	programs = TotalPrograms();

	IssueIo(2000, IO_NVM_WRITE);

	TEST_ASSERT_EQUAL_UINT32(programs, TotalPrograms());
	TEST_ASSERT_EQUAL_HEX32(VSA_FAIL, AddrTransRead(1000));
}

static void test_rewrite_of_buffered_lsa_stays_dirty_without_extra_program(void)
{
	unsigned int programs;

	IssueIo(55, IO_NVM_WRITE);
	programs = TotalPrograms();
	IssueIo(55, IO_NVM_WRITE);

	TEST_ASSERT_EQUAL_UINT32(programs, TotalPrograms());
	TEST_ASSERT_EQUAL_UINT32(DATA_BUF_DIRTY, dataBufMapPtr->dataBuf[FindEntry(55)].dirty);
	TEST_ASSERT_EQUAL_UINT32(2 * NVME_BLOCKS_PER_SLICE,
							 fake_dma_count_of(HOST_DMA_AUTO_TYPE, HOST_DMA_RX_DIRECTION));
}

/* ---- Temporary (GC) buffers --------------------------------------------- */

static void test_temp_buffer_is_one_fixed_entry_per_die(void)
{
	unsigned int die;
	for (die = 0; die < USER_DIES; die++)
	{
		TEST_ASSERT_EQUAL_UINT32(die, AllocateTempDataBuf(die));
		TEST_ASSERT_EQUAL_UINT32(REQ_SLOT_TAG_NONE, tempDataBufMapPtr->tempDataBuf[die].blockingReqTail);
	}
}

static void test_blocking_req_chain_links_requests_in_order(void)
{
	unsigned int t1 = GetFromFreeReqQ(), t2 = GetFromFreeReqQ();

	UpdateDataBufEntryInfoBlockingReq(3, t1);
	TEST_ASSERT_EQUAL_UINT32(t1, dataBufMapPtr->dataBuf[3].blockingReqTail);
	UpdateDataBufEntryInfoBlockingReq(3, t2);
	TEST_ASSERT_EQUAL_UINT32(t2, dataBufMapPtr->dataBuf[3].blockingReqTail);
	TEST_ASSERT_EQUAL_UINT32(t1, reqPoolPtr->reqPool[t2].prevBlockingReq);
	TEST_ASSERT_EQUAL_UINT32(t2, reqPoolPtr->reqPool[t1].nextBlockingReq);

	dataBufMapPtr->dataBuf[3].blockingReqTail = REQ_SLOT_TAG_NONE;
	PutToFreeReqQ(t1);
	PutToFreeReqQ(t2);
}

int main(void)
{
	UNITY_BEGIN();
	RUN_TEST(test_init_links_every_entry_into_lru_and_no_hash_chains);
	RUN_TEST(test_lookup_misses_for_unseen_lsa);
	RUN_TEST(test_miss_allocates_lru_tail_and_hit_finds_same_entry);
	RUN_TEST(test_read_hit_is_served_from_buffer_without_nand_access);
	RUN_TEST(test_read_miss_of_flashed_lsa_fetches_from_nand);
	RUN_TEST(test_read_miss_of_never_written_lsa_skips_nand);
	RUN_TEST(test_colliding_lsas_share_hash_chain_and_both_hit);
	RUN_TEST(test_hit_moves_entry_to_lru_head);
	RUN_TEST(test_eviction_takes_least_recently_used_entry);
	RUN_TEST(test_eviction_leaves_hash_chains_consistent);
	RUN_TEST(test_dirty_eviction_writes_victim_back_to_nand);
	RUN_TEST(test_clean_eviction_does_not_touch_nand);
	RUN_TEST(test_rewrite_of_buffered_lsa_stays_dirty_without_extra_program);
	RUN_TEST(test_temp_buffer_is_one_fixed_entry_per_die);
	RUN_TEST(test_blocking_req_chain_links_requests_in_order);
	return UNITY_END();
}
