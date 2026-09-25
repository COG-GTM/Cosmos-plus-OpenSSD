#include "test_support.h"

void setUp(void) { TestFtlReset(); }
void tearDown(void) {}

#define ENTRIES AVAILABLE_DATA_BUFFER_ENTRY_COUNT

static unsigned int NewSliceReq(unsigned int lsa, unsigned int reqCode)
{
	unsigned int tag = GetFromFreeReqQ();
	reqPoolPtr->reqPool[tag].reqType = REQ_TYPE_SLICE;
	reqPoolPtr->reqPool[tag].reqCode = reqCode;
	reqPoolPtr->reqPool[tag].nvmeCmdSlotTag = 0;
	reqPoolPtr->reqPool[tag].logicalSliceAddr = lsa;
	reqPoolPtr->reqPool[tag].nvmeDmaInfo.startIndex = 0;
	reqPoolPtr->reqPool[tag].nvmeDmaInfo.nvmeBlockOffset = 0;
	reqPoolPtr->reqPool[tag].nvmeDmaInfo.numOfNvmeBlock = NVME_BLOCKS_PER_SLICE;
	return tag;
}

/* Allocates the LRU entry and binds it to `lsa` the same way ReqTransSliceToLowLevel() does. */
static unsigned int Cache(unsigned int lsa)
{
	unsigned int entry = AllocateDataBuf();
	dataBufMapPtr->dataBuf[entry].logicalSliceAddr = lsa;
	PutToDataBufHashList(entry);
	return entry;
}

static unsigned int LruLength(void)
{
	unsigned int e = dataBufLruList.headEntry, n = 0;
	while (e != DATA_BUF_NONE)
	{
		n++;
		e = dataBufMapPtr->dataBuf[e].nextEntry;
	}
	return n;
}

/* Counts live requests (in any queue: NAND, blocked-by-dependency, DMA) with the given code and LSA. */
static unsigned int CountLiveReqsForLsa(unsigned int reqCode, unsigned int lsa)
{
	unsigned int tag, n = 0;
	for (tag = 0; tag < AVAILABLE_OUNTSTANDING_REQ_COUNT; tag++)
		if (reqPoolPtr->reqPool[tag].reqQueueType != REQ_QUEUE_TYPE_FREE
			&& reqPoolPtr->reqPool[tag].reqCode == reqCode
			&& reqPoolPtr->reqPool[tag].logicalSliceAddr == lsa)
			n++;
	return n;
}

/* ---------------------------------------------------------------- init */

void test_init_links_every_entry_into_lru_list_clean_and_unmapped(void)
{
	unsigned int e;
	TEST_ASSERT_EQUAL_UINT(0, dataBufLruList.headEntry);
	TEST_ASSERT_EQUAL_UINT(ENTRIES - 1, dataBufLruList.tailEntry);
	TEST_ASSERT_EQUAL_UINT(ENTRIES, LruLength());
	for (e = 0; e < ENTRIES; e++)
	{
		TEST_ASSERT_EQUAL_HEX32(LSA_NONE, dataBufMapPtr->dataBuf[e].logicalSliceAddr);
		TEST_ASSERT_EQUAL_UINT(DATA_BUF_CLEAN, dataBufMapPtr->dataBuf[e].dirty);
		TEST_ASSERT_EQUAL_UINT(DATA_BUF_NONE, dataBufHashTablePtr->dataBufHash[e].headEntry);
	}
}

/* ---------------------------------------------------------------- hit / miss */

void test_lookup_misses_when_lsa_not_cached(void)
{
	unsigned int tag = NewSliceReq(123, REQ_CODE_READ);
	TEST_ASSERT_EQUAL_UINT(DATA_BUF_FAIL, CheckDataBufHit(tag));
}

void test_lookup_hits_cached_lsa_and_promotes_it_to_mru(void)
{
	unsigned int entry = Cache(123);
	unsigned int tag = NewSliceReq(123, REQ_CODE_READ);

	TEST_ASSERT_EQUAL_UINT(ENTRIES - 1, entry);            /* allocation takes the LRU tail */
	TEST_ASSERT_EQUAL_UINT(entry, dataBufLruList.headEntry);

	Cache(200);                                             /* somebody else becomes MRU */
	TEST_ASSERT_NOT_EQUAL(entry, dataBufLruList.headEntry);

	TEST_ASSERT_EQUAL_UINT(entry, CheckDataBufHit(tag));
	TEST_ASSERT_EQUAL_UINT(entry, dataBufLruList.headEntry);
	TEST_ASSERT_EQUAL_UINT(ENTRIES, LruLength());
}

void test_lookup_distinguishes_lsas_sharing_a_hash_bucket(void)
{
	unsigned int lsaA = 7, lsaB = 7 + ENTRIES, lsaC = 7 + 2 * ENTRIES;
	unsigned int a = Cache(lsaA), b = Cache(lsaB);
	unsigned int tagA = NewSliceReq(lsaA, REQ_CODE_READ);
	unsigned int tagB = NewSliceReq(lsaB, REQ_CODE_READ);
	unsigned int tagC = NewSliceReq(lsaC, REQ_CODE_READ);

	TEST_ASSERT_EQUAL_UINT(FindDataBufHashTableEntry(lsaA), FindDataBufHashTableEntry(lsaB));
	TEST_ASSERT_EQUAL_UINT(a, CheckDataBufHit(tagA));
	TEST_ASSERT_EQUAL_UINT(b, CheckDataBufHit(tagB));
	TEST_ASSERT_EQUAL_UINT(DATA_BUF_FAIL, CheckDataBufHit(tagC));
}

void test_lookup_misses_after_entry_is_reused_for_another_lsa(void)
{
	unsigned int i;
	unsigned int tag = NewSliceReq(5, REQ_CODE_READ);

	Cache(5);
	for (i = 0; i < ENTRIES; i++)   /* cycle the whole cache */
		Cache(1000 + i);

	TEST_ASSERT_EQUAL_UINT(DATA_BUF_FAIL, CheckDataBufHit(tag));
	TEST_ASSERT_EQUAL_UINT(ENTRIES, LruLength());
}

/* ---------------------------------------------------------------- LRU order */

void test_allocation_evicts_least_recently_used_entry(void)
{
	unsigned int i, first, second;

	first = Cache(0);
	second = Cache(1);
	for (i = 2; i < ENTRIES; i++)
		Cache(i);

	/* Cache is full; the next allocation must recycle `first` (oldest), then `second`. */
	TEST_ASSERT_EQUAL_UINT(first, Cache(ENTRIES));
	TEST_ASSERT_EQUAL_UINT(second, Cache(ENTRIES + 1));
	TEST_ASSERT_EQUAL_UINT(ENTRIES, LruLength());
}

void test_hit_protects_entry_from_eviction(void)
{
	unsigned int i, first, second, tag;

	first = Cache(0);
	second = Cache(1);
	for (i = 2; i < ENTRIES; i++)
		Cache(i);

	tag = NewSliceReq(0, REQ_CODE_READ);
	TEST_ASSERT_EQUAL_UINT(first, CheckDataBufHit(tag));   /* touch LSA 0 */

	TEST_ASSERT_EQUAL_UINT(second, Cache(ENTRIES));        /* LSA 1 goes first now */
	TEST_ASSERT_NOT_EQUAL(first, Cache(ENTRIES + 1));
}

void test_allocation_unlinks_entry_from_hash_chain(void)
{
	unsigned int entry = Cache(9);
	unsigned int bucket = FindDataBufHashTableEntry(9);
	unsigned int i;

	TEST_ASSERT_EQUAL_UINT(entry, dataBufHashTablePtr->dataBufHash[bucket].headEntry);
	for (i = 0; i < ENTRIES; i++)
		AllocateDataBuf();                                  /* recycle everything, incl. `entry` */
	TEST_ASSERT_EQUAL_UINT(DATA_BUF_NONE, dataBufHashTablePtr->dataBufHash[bucket].headEntry);
	TEST_ASSERT_EQUAL_UINT(DATA_BUF_NONE, dataBufHashTablePtr->dataBufHash[bucket].tailEntry);
}

/* ---------------------------------------------------------------- dirty eviction */

void test_evicting_clean_entry_issues_no_nand_write(void)
{
	unsigned int entry = Cache(42);
	unsigned int tag = NewSliceReq(43, REQ_CODE_WRITE);
	unsigned int reqsBefore = freeReqQ.reqCnt;

	reqPoolPtr->reqPool[tag].dataBufInfo.entry = entry;
	EvictDataBufEntry(tag);

	TEST_ASSERT_EQUAL_UINT(reqsBefore, freeReqQ.reqCnt);
	TEST_ASSERT_EQUAL_HEX32(VSA_FAIL, AddrTransRead(42));
}

void test_evicting_dirty_entry_writes_it_back_to_nand(void)
{
	unsigned int lsa = 42;
	unsigned int entry = Cache(lsa);
	unsigned int tag = NewSliceReq(43, REQ_CODE_WRITE);
	unsigned int reqsBefore = freeReqQ.reqCnt;
	unsigned int vsa;

	dataBufMapPtr->dataBuf[entry].dirty = DATA_BUF_DIRTY;
	reqPoolPtr->reqPool[tag].dataBufInfo.entry = entry;
	EvictDataBufEntry(tag);

	TEST_ASSERT_EQUAL_UINT(reqsBefore - 1, freeReqQ.reqCnt);
	TEST_ASSERT_EQUAL_UINT(DATA_BUF_CLEAN, dataBufMapPtr->dataBuf[entry].dirty);

	vsa = AddrTransRead(lsa);
	TEST_ASSERT_NOT_EQUAL(VSA_FAIL, vsa);
	TEST_ASSERT_EQUAL_UINT(1, CountLiveReqsForLsa(REQ_CODE_WRITE, lsa));
	TEST_ASSERT_EQUAL_UINT(vsa, reqPoolPtr->reqPool[dataBufMapPtr->dataBuf[entry].blockingReqTail].nandInfo.virtualSliceAddr);
	TEST_ASSERT_EQUAL_UINT(entry, reqPoolPtr->reqPool[dataBufMapPtr->dataBuf[entry].blockingReqTail].dataBufInfo.entry);
}

/* ---------------------------------------------------------------- through the request path */

void test_write_request_marks_buffer_dirty_and_queues_rx_dma(void)
{
	unsigned int tag = NewSliceReq(77, REQ_CODE_WRITE);
	unsigned int entry;

	PutToSliceReqQ(tag);
	ReqTransSliceToLowLevel();

	entry = reqPoolPtr->reqPool[tag].dataBufInfo.entry;
	TEST_ASSERT_EQUAL_UINT(77, dataBufMapPtr->dataBuf[entry].logicalSliceAddr);
	TEST_ASSERT_EQUAL_UINT(DATA_BUF_DIRTY, dataBufMapPtr->dataBuf[entry].dirty);
	TEST_ASSERT_EQUAL_UINT(REQ_CODE_RxDMA, reqPoolPtr->reqPool[tag].reqCode);
	TEST_ASSERT_EQUAL_UINT(REQ_TYPE_NVME_DMA, reqPoolPtr->reqPool[tag].reqType);
	TEST_ASSERT_EQUAL_UINT(NVME_BLOCKS_PER_SLICE, FakeHostDmaCountFor(HOST_DMA_AUTO_TYPE, HOST_DMA_RX_DIRECTION));
	TEST_ASSERT_EQUAL_UINT(0, FakeHostDmaCountFor(HOST_DMA_AUTO_TYPE, HOST_DMA_TX_DIRECTION));
	/* A full-slice write needs no read-modify-write fetch from NAND. */
	TEST_ASSERT_EQUAL_UINT(0, CountLiveReqsForLsa(REQ_CODE_READ, 77));
}

void test_partial_write_miss_triggers_read_modify_write(void)
{
	unsigned int tag;

	TestWriteLogicalSlices(77, 1);                            /* LSA 77 exists on NAND */
	tag = NewSliceReq(77, REQ_CODE_WRITE);
	reqPoolPtr->reqPool[tag].nvmeDmaInfo.numOfNvmeBlock = 1;

	PutToSliceReqQ(tag);
	ReqTransSliceToLowLevel();

	TEST_ASSERT_EQUAL_UINT(1, CountLiveReqsForLsa(REQ_CODE_READ, 77));
	/* The host->buffer DMA must wait for the NAND read of the rest of the slice. */
	TEST_ASSERT_EQUAL_UINT(REQ_QUEUE_TYPE_BLOCKED_BY_BUF_DEP, reqPoolPtr->reqPool[tag].reqQueueType);
	TEST_ASSERT_EQUAL_UINT(0, FakeHostDmaCountFor(HOST_DMA_AUTO_TYPE, HOST_DMA_RX_DIRECTION));
}

void test_read_hit_reuses_dirty_buffer_without_touching_nand(void)
{
	unsigned int write = NewSliceReq(88, REQ_CODE_WRITE);
	unsigned int read = NewSliceReq(88, REQ_CODE_READ);
	unsigned int reqsAfterWrite;

	PutToSliceReqQ(write);
	ReqTransSliceToLowLevel();
	reqsAfterWrite = freeReqQ.reqCnt;

	PutToSliceReqQ(read);
	ReqTransSliceToLowLevel();

	TEST_ASSERT_EQUAL_UINT(reqPoolPtr->reqPool[write].dataBufInfo.entry, reqPoolPtr->reqPool[read].dataBufInfo.entry);
	TEST_ASSERT_EQUAL_UINT(REQ_CODE_TxDMA, reqPoolPtr->reqPool[read].reqCode);
	TEST_ASSERT_EQUAL_UINT(reqsAfterWrite, freeReqQ.reqCnt);  /* no extra NAND request */
	TEST_ASSERT_EQUAL_UINT(0, CountLiveReqsForLsa(REQ_CODE_READ, 88));
	TEST_ASSERT_EQUAL_HEX32(VSA_FAIL, AddrTransRead(88));      /* still only in the buffer */
}

void test_read_miss_of_unwritten_lsa_only_streams_buffer_to_host(void)
{
	unsigned int read = NewSliceReq(99, REQ_CODE_READ);

	PutToSliceReqQ(read);
	ReqTransSliceToLowLevel();

	TEST_ASSERT_EQUAL_UINT(REQ_CODE_TxDMA, reqPoolPtr->reqPool[read].reqCode);
	TEST_ASSERT_EQUAL_UINT(NVME_BLOCKS_PER_SLICE, FakeHostDmaCountFor(HOST_DMA_AUTO_TYPE, HOST_DMA_TX_DIRECTION));
	TEST_ASSERT_EQUAL_UINT(0, CountLiveReqsForLsa(REQ_CODE_READ, 99));
}

void test_streaming_writes_evict_dirty_entries_in_lru_order(void)
{
	unsigned int i, tag;
	unsigned int lsaBase = 500;

	for (i = 0; i < ENTRIES; i++)
	{
		tag = NewSliceReq(lsaBase + i, REQ_CODE_WRITE);
		PutToSliceReqQ(tag);
	}
	ReqTransSliceToLowLevel();
	TEST_ASSERT_EQUAL_HEX32(VSA_FAIL, AddrTransRead(lsaBase));

	/* One more write: the oldest dirty slice (lsaBase) must be flushed, nothing else. */
	tag = NewSliceReq(lsaBase + ENTRIES, REQ_CODE_WRITE);
	PutToSliceReqQ(tag);
	ReqTransSliceToLowLevel();

	TEST_ASSERT_NOT_EQUAL(VSA_FAIL, AddrTransRead(lsaBase));
	TEST_ASSERT_EQUAL_HEX32(VSA_FAIL, AddrTransRead(lsaBase + 1));
	TEST_ASSERT_EQUAL_UINT(1, CountLiveReqsForLsa(REQ_CODE_WRITE, lsaBase));
}

/* ---------------------------------------------------------------- temp buffers */

void test_temp_data_buf_is_one_per_die(void)
{
	unsigned int die;
	for (die = 0; die < USER_DIES; die++)
	{
		TEST_ASSERT_EQUAL_UINT(die, AllocateTempDataBuf(die));
		TEST_ASSERT_EQUAL_UINT(REQ_SLOT_TAG_NONE, tempDataBufMapPtr->tempDataBuf[die].blockingReqTail);
	}
}

void test_blocking_req_chain_is_appended_in_order(void)
{
	unsigned int a = GetFromFreeReqQ(), b = GetFromFreeReqQ();

	UpdateTempDataBufEntryInfoBlockingReq(0, a);
	UpdateTempDataBufEntryInfoBlockingReq(0, b);
	TEST_ASSERT_EQUAL_UINT(b, tempDataBufMapPtr->tempDataBuf[0].blockingReqTail);
	TEST_ASSERT_EQUAL_UINT(a, reqPoolPtr->reqPool[b].prevBlockingReq);
	TEST_ASSERT_EQUAL_UINT(b, reqPoolPtr->reqPool[a].nextBlockingReq);

	UpdateDataBufEntryInfoBlockingReq(3, a);
	UpdateDataBufEntryInfoBlockingReq(3, b);
	TEST_ASSERT_EQUAL_UINT(b, dataBufMapPtr->dataBuf[3].blockingReqTail);
}

int main(void)
{
	UNITY_BEGIN();
	RUN_TEST(test_init_links_every_entry_into_lru_list_clean_and_unmapped);
	RUN_TEST(test_lookup_misses_when_lsa_not_cached);
	RUN_TEST(test_lookup_hits_cached_lsa_and_promotes_it_to_mru);
	RUN_TEST(test_lookup_distinguishes_lsas_sharing_a_hash_bucket);
	RUN_TEST(test_lookup_misses_after_entry_is_reused_for_another_lsa);
	RUN_TEST(test_allocation_evicts_least_recently_used_entry);
	RUN_TEST(test_hit_protects_entry_from_eviction);
	RUN_TEST(test_allocation_unlinks_entry_from_hash_chain);
	RUN_TEST(test_evicting_clean_entry_issues_no_nand_write);
	RUN_TEST(test_evicting_dirty_entry_writes_it_back_to_nand);
	RUN_TEST(test_write_request_marks_buffer_dirty_and_queues_rx_dma);
	RUN_TEST(test_partial_write_miss_triggers_read_modify_write);
	RUN_TEST(test_read_hit_reuses_dirty_buffer_without_touching_nand);
	RUN_TEST(test_read_miss_of_unwritten_lsa_only_streams_buffer_to_host);
	RUN_TEST(test_streaming_writes_evict_dirty_entries_in_lru_order);
	RUN_TEST(test_temp_data_buf_is_one_per_die);
	RUN_TEST(test_blocking_req_chain_is_appended_in_order);
	return UNITY_END();
}
