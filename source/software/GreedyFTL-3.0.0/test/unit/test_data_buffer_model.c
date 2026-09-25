/* Model-based and boundary tests for data_buffer.c: the LRU list and hash
 * table are checked against a simple reference model over long, seeded
 * operation sequences. */
#include "unity.h"

#include "fw_test.h"
#include "memory_map.h"

extern P_DATA_BUF_HASH_TABLE dataBufHashTablePtr;

#define BUF_COUNT AVAILABLE_DATA_BUFFER_ENTRY_COUNT
#define LAST_BUF (BUF_COUNT - 1)
#define MODEL_OPS 20000
#define MODEL_LSA_SPACE (4 * BUF_COUNT)
#define MODEL_CHECK_INTERVAL 997

/* Reference model: modelOrder[0] is the LRU head, modelOrder[BUF_COUNT-1] the tail. */
static unsigned int modelOrder[BUF_COUNT];
static unsigned int modelLsa[BUF_COUNT];
static unsigned int rngState;

void setUp(void)
{
	fw_test_reset();
	fw_test_init_ftl();
}

void tearDown(void) {}

/* ---------- helpers ---------- */

static unsigned int NextRandom(void)
{
	rngState = rngState * 1103515245u + 12345u;
	return (rngState >> 8) & 0xffffff;
}

static void InsertBufWithLsa(unsigned int bufEntry, unsigned int logicalSliceAddr)
{
	dataBufMapPtr->dataBuf[bufEntry].logicalSliceAddr = logicalSliceAddr;
	PutToDataBufHashList(bufEntry);
}

static unsigned int Lookup(unsigned int reqSlotTag, unsigned int logicalSliceAddr)
{
	reqPoolPtr->reqPool[reqSlotTag].logicalSliceAddr = logicalSliceAddr;
	return CheckDataBufHit(reqSlotTag);
}

static void ModelInit(void)
{
	unsigned int i;

	for (i = 0; i < BUF_COUNT; i++)
	{
		modelOrder[i] = i;
		modelLsa[i] = LSA_NONE;
	}
}

static void ModelMoveToHead(unsigned int bufEntry)
{
	unsigned int pos = 0;

	while (modelOrder[pos] != bufEntry)
		pos++;
	for (; pos > 0; pos--)
		modelOrder[pos] = modelOrder[pos - 1];
	modelOrder[0] = bufEntry;
}

static unsigned int ModelFindLsa(unsigned int logicalSliceAddr)
{
	unsigned int i;

	for (i = 0; i < BUF_COUNT; i++)
		if (modelLsa[i] == logicalSliceAddr)
			return i;
	return DATA_BUF_NONE;
}

static void AssertLruMatchesModel(void)
{
	unsigned int i;
	unsigned int prev = DATA_BUF_NONE;
	unsigned int cur = dataBufLruList.headEntry;

	for (i = 0; i < BUF_COUNT; i++)
	{
		TEST_ASSERT_EQUAL_UINT(modelOrder[i], cur);
		TEST_ASSERT_EQUAL_UINT(prev, dataBufMapPtr->dataBuf[cur].prevEntry);
		prev = cur;
		cur = dataBufMapPtr->dataBuf[cur].nextEntry;
	}
	TEST_ASSERT_EQUAL_UINT(DATA_BUF_NONE, cur);
	TEST_ASSERT_EQUAL_UINT(modelOrder[LAST_BUF], dataBufLruList.tailEntry);
}

static unsigned int HashChainLength(unsigned int hashEntry)
{
	unsigned int count = 0;
	unsigned int prev = DATA_BUF_NONE;
	unsigned int cur = dataBufHashTablePtr->dataBufHash[hashEntry].headEntry;

	while (cur != DATA_BUF_NONE)
	{
		TEST_ASSERT_NOT_EQUAL_MESSAGE(LSA_NONE, dataBufMapPtr->dataBuf[cur].logicalSliceAddr,
								"non-resident entry in a hash chain");
		TEST_ASSERT_EQUAL_UINT(prev, dataBufMapPtr->dataBuf[cur].hashPrevEntry);
		TEST_ASSERT_EQUAL_UINT(hashEntry, FindDataBufHashTableEntry(dataBufMapPtr->dataBuf[cur].logicalSliceAddr));
		prev = cur;
		cur = dataBufMapPtr->dataBuf[cur].hashNextEntry;
		count++;
		TEST_ASSERT_TRUE_MESSAGE(count <= BUF_COUNT, "hash chain has a cycle");
	}
	TEST_ASSERT_EQUAL_UINT(prev, dataBufHashTablePtr->dataBufHash[hashEntry].tailEntry);
	return count;
}

static void AssertHashMatchesModel(void)
{
	unsigned int hashEntry, bufEntry;
	unsigned int chained = 0, expected = 0;

	for (hashEntry = 0; hashEntry < BUF_COUNT; hashEntry++)
		chained += HashChainLength(hashEntry);
	for (bufEntry = 0; bufEntry < BUF_COUNT; bufEntry++)
	{
		TEST_ASSERT_EQUAL_HEX32(modelLsa[bufEntry], dataBufMapPtr->dataBuf[bufEntry].logicalSliceAddr);
		if (modelLsa[bufEntry] != LSA_NONE)
			expected++;
	}
	TEST_ASSERT_EQUAL_UINT(expected, chained);
}

/* ---------- model-based sequences ---------- */

static void RunModelSequence(unsigned int seed)
{
	unsigned int reqSlotTag = GetFromFreeReqQ();
	unsigned int op;

	rngState = seed;
	ModelInit();

	for (op = 0; op < MODEL_OPS; op++)
	{
		unsigned int choice = NextRandom() % 3;
		unsigned int lsa = NextRandom() % MODEL_LSA_SPACE;
		unsigned int owner = ModelFindLsa(lsa);

		if (choice == 0)
		{
			unsigned int victim = modelOrder[LAST_BUF];

			TEST_ASSERT_EQUAL_UINT(victim, AllocateDataBuf());
			ModelMoveToHead(victim);
			modelLsa[victim] = LSA_NONE;
			dataBufMapPtr->dataBuf[victim].logicalSliceAddr = LSA_NONE;
			if (owner == DATA_BUF_NONE)
			{
				InsertBufWithLsa(victim, lsa);
				modelLsa[victim] = lsa;
			}
		}
		else if (owner != DATA_BUF_NONE)
		{
			TEST_ASSERT_EQUAL_UINT(owner, Lookup(reqSlotTag, lsa));
			ModelMoveToHead(owner);
		}
		else
		{
			TEST_ASSERT_EQUAL_HEX32(DATA_BUF_FAIL, Lookup(reqSlotTag, lsa));
		}

		if ((op % MODEL_CHECK_INTERVAL) == 0)
		{
			AssertLruMatchesModel();
			AssertHashMatchesModel();
		}
	}

	AssertLruMatchesModel();
	AssertHashMatchesModel();
}

static void test_model_random_ops_seed_1(void)
{
	RunModelSequence(1);
}

static void test_model_random_ops_seed_0xc0ffee(void)
{
	RunModelSequence(0xc0ffee);
}

static void test_hits_in_ascending_order_make_eviction_ascending(void)
{
	unsigned int reqSlotTag = GetFromFreeReqQ();
	unsigned int i;

	for (i = 0; i < BUF_COUNT; i++)
		InsertBufWithLsa(i, i);
	for (i = 0; i < BUF_COUNT; i++)
		TEST_ASSERT_EQUAL_UINT(i, Lookup(reqSlotTag, i));

	TEST_ASSERT_EQUAL_UINT(LAST_BUF, dataBufLruList.headEntry);
	TEST_ASSERT_EQUAL_UINT(0, dataBufLruList.tailEntry);

	for (i = 0; i < BUF_COUNT; i++)
	{
		TEST_ASSERT_EQUAL_UINT(i, AllocateDataBuf());
		TEST_ASSERT_EQUAL_UINT(0, HashChainLength(FindDataBufHashTableEntry(i)));
	}
}

/* ---------- hash table boundaries ---------- */

static void test_boundary_lsas_land_in_expected_buckets_and_hit(void)
{
	static const unsigned int lsas[] = { 0, BUF_COUNT - 1, BUF_COUNT, 0xfffffffeu };
	unsigned int reqSlotTag = GetFromFreeReqQ();
	unsigned int i;

	for (i = 0; i < 4; i++)
		InsertBufWithLsa(i + 1, lsas[i]);

	TEST_ASSERT_EQUAL_UINT(2, HashChainLength(0));
	TEST_ASSERT_EQUAL_UINT(1, HashChainLength(LAST_BUF));
	TEST_ASSERT_EQUAL_UINT(1, dataBufHashTablePtr->dataBufHash[0].headEntry);
	TEST_ASSERT_EQUAL_UINT(3, dataBufHashTablePtr->dataBufHash[0].tailEntry);
	TEST_ASSERT_EQUAL_UINT(1, HashChainLength(0xfffffffeu % BUF_COUNT));

	for (i = 0; i < 4; i++)
		TEST_ASSERT_EQUAL_UINT(i + 1, Lookup(reqSlotTag, lsas[i]));
	TEST_ASSERT_EQUAL_HEX32(DATA_BUF_FAIL, Lookup(reqSlotTag, 2 * BUF_COUNT));
}

static void test_single_bucket_holding_every_entry(void)
{
	unsigned int reqSlotTag = GetFromFreeReqQ();
	unsigned int hashEntry = 7;
	unsigned int i;

	for (i = 0; i < BUF_COUNT; i++)
		InsertBufWithLsa(i, hashEntry + i * BUF_COUNT);
	TEST_ASSERT_EQUAL_UINT(BUF_COUNT, HashChainLength(hashEntry));

	/* Hit on the chain tail walks every colliding entry. */
	TEST_ASSERT_EQUAL_UINT(LAST_BUF, Lookup(reqSlotTag, hashEntry + LAST_BUF * BUF_COUNT));

	/* Remove head, tail and every other middle entry. */
	for (i = 0; i < BUF_COUNT; i += 2)
		SelectiveGetFromDataBufHashList(i);
	SelectiveGetFromDataBufHashList(LAST_BUF);
	for (i = 0; i < BUF_COUNT; i += 2)
		dataBufMapPtr->dataBuf[i].logicalSliceAddr = LSA_NONE;
	dataBufMapPtr->dataBuf[LAST_BUF].logicalSliceAddr = LSA_NONE;

	TEST_ASSERT_EQUAL_UINT(BUF_COUNT / 2 - 1, HashChainLength(hashEntry));
	TEST_ASSERT_EQUAL_UINT(1, dataBufHashTablePtr->dataBufHash[hashEntry].headEntry);
	TEST_ASSERT_EQUAL_UINT(LAST_BUF - 2, dataBufHashTablePtr->dataBufHash[hashEntry].tailEntry);
	TEST_ASSERT_EQUAL_UINT(3, Lookup(reqSlotTag, hashEntry + 3 * BUF_COUNT));
	TEST_ASSERT_EQUAL_HEX32(DATA_BUF_FAIL, Lookup(reqSlotTag, hashEntry + 2 * BUF_COUNT));

	for (i = 1; i < LAST_BUF; i += 2)
		SelectiveGetFromDataBufHashList(i);
	TEST_ASSERT_EQUAL_UINT(0, HashChainLength(hashEntry));
	TEST_ASSERT_EQUAL_UINT(DATA_BUF_NONE, dataBufHashTablePtr->dataBufHash[hashEntry].headEntry);
	TEST_ASSERT_EQUAL_UINT(DATA_BUF_NONE, dataBufHashTablePtr->dataBufHash[hashEntry].tailEntry);
}

/* ---------- re-initialisation ---------- */

static void test_reinit_after_use_restores_initial_state(void)
{
	unsigned int reqSlotTag = GetFromFreeReqQ();
	unsigned int i;

	InsertBufWithLsa(LAST_BUF, 11);
	InsertBufWithLsa(3, 11 + BUF_COUNT);
	TEST_ASSERT_EQUAL_UINT(3, Lookup(reqSlotTag, 11 + BUF_COUNT));
	AllocateDataBuf();
	UpdateDataBufEntryInfoBlockingReq(3, reqSlotTag);
	UpdateTempDataBufEntryInfoBlockingReq(0, reqSlotTag);
	dataBufMapPtr->dataBuf[3].dirty = DATA_BUF_DIRTY;

	InitDataBuf();

	ModelInit();
	AssertLruMatchesModel();
	AssertHashMatchesModel();
	for (i = 0; i < BUF_COUNT; i++)
	{
		TEST_ASSERT_EQUAL_UINT(DATA_BUF_CLEAN, dataBufMapPtr->dataBuf[i].dirty);
		TEST_ASSERT_EQUAL_UINT(REQ_SLOT_TAG_NONE, dataBufMapPtr->dataBuf[i].blockingReqTail);
	}
	TEST_ASSERT_EQUAL_UINT(REQ_SLOT_TAG_NONE, tempDataBufMapPtr->tempDataBuf[0].blockingReqTail);
	TEST_ASSERT_EQUAL_HEX32(DATA_BUF_FAIL, Lookup(reqSlotTag, 11 + BUF_COUNT));
}

/* ---------- blocking chains at field boundaries ---------- */

static void test_blocking_chain_holds_highest_req_slot_tag(void)
{
	unsigned int lowTag = 0;
	unsigned int highTag = AVAILABLE_OUNTSTANDING_REQ_COUNT - 1;

	reqPoolPtr->reqPool[lowTag].nextBlockingReq = REQ_SLOT_TAG_NONE;
	reqPoolPtr->reqPool[highTag].prevBlockingReq = REQ_SLOT_TAG_NONE;

	UpdateDataBufEntryInfoBlockingReq(LAST_BUF, lowTag);
	UpdateDataBufEntryInfoBlockingReq(LAST_BUF, highTag);

	TEST_ASSERT_EQUAL_UINT(highTag, dataBufMapPtr->dataBuf[LAST_BUF].blockingReqTail);
	TEST_ASSERT_EQUAL_UINT(lowTag, reqPoolPtr->reqPool[highTag].prevBlockingReq);
	TEST_ASSERT_EQUAL_UINT(highTag, reqPoolPtr->reqPool[lowTag].nextBlockingReq);
	/* Neighbouring entries are untouched. */
	TEST_ASSERT_EQUAL_UINT(REQ_SLOT_TAG_NONE, dataBufMapPtr->dataBuf[LAST_BUF - 1].blockingReqTail);
}

static void test_temp_blocking_chains_are_independent_per_die(void)
{
	unsigned int firstDie = AllocateTempDataBuf(0);
	unsigned int lastDie = AllocateTempDataBuf(USER_DIES - 1);
	unsigned int reqA = GetFromFreeReqQ();
	unsigned int reqB = GetFromFreeReqQ();
	unsigned int reqC = GetFromFreeReqQ();

	UpdateTempDataBufEntryInfoBlockingReq(firstDie, reqA);
	UpdateTempDataBufEntryInfoBlockingReq(lastDie, reqB);
	UpdateTempDataBufEntryInfoBlockingReq(firstDie, reqC);

	TEST_ASSERT_EQUAL_UINT(reqC, tempDataBufMapPtr->tempDataBuf[firstDie].blockingReqTail);
	TEST_ASSERT_EQUAL_UINT(reqB, tempDataBufMapPtr->tempDataBuf[lastDie].blockingReqTail);
	TEST_ASSERT_EQUAL_UINT(reqA, reqPoolPtr->reqPool[reqC].prevBlockingReq);
	TEST_ASSERT_EQUAL_UINT(reqC, reqPoolPtr->reqPool[reqA].nextBlockingReq);
	TEST_ASSERT_EQUAL_UINT(REQ_SLOT_TAG_NONE, reqPoolPtr->reqPool[reqB].prevBlockingReq);
	TEST_ASSERT_EQUAL_UINT(REQ_SLOT_TAG_NONE, reqPoolPtr->reqPool[reqB].nextBlockingReq);
}

int main(void)
{
	UNITY_BEGIN();
	RUN_TEST(test_model_random_ops_seed_1);
	RUN_TEST(test_model_random_ops_seed_0xc0ffee);
	RUN_TEST(test_hits_in_ascending_order_make_eviction_ascending);

	RUN_TEST(test_boundary_lsas_land_in_expected_buckets_and_hit);
	RUN_TEST(test_single_bucket_holding_every_entry);

	RUN_TEST(test_reinit_after_use_restores_initial_state);

	RUN_TEST(test_blocking_chain_holds_highest_req_slot_tag);
	RUN_TEST(test_temp_blocking_chains_are_independent_per_die);
	return UNITY_END();
}
