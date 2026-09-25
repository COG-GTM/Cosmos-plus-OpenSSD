/* Unit tests for data_buffer.c. */
#include "unity.h"

#include "fw_test.h"
#include "memory_map.h"

void setUp(void)
{
	fw_test_reset();
	fw_test_init_ftl();
}

void tearDown(void) {}

static void test_smoke_lookup_misses_on_empty_buffer(void)
{
	unsigned int reqSlotTag = GetFromFreeReqQ();

	reqPoolPtr->reqPool[reqSlotTag].logicalSliceAddr = 42;

	TEST_ASSERT_EQUAL_HEX32(DATA_BUF_FAIL, CheckDataBufHit(reqSlotTag));
}

static void test_smoke_allocated_entry_hits_after_hash_insert(void)
{
	unsigned int reqSlotTag = GetFromFreeReqQ();
	unsigned int bufEntry = AllocateDataBuf();

	TEST_ASSERT_NOT_EQUAL(DATA_BUF_FAIL, bufEntry);
	dataBufMapPtr->dataBuf[bufEntry].logicalSliceAddr = 42;
	PutToDataBufHashList(bufEntry);
	reqPoolPtr->reqPool[reqSlotTag].logicalSliceAddr = 42;

	TEST_ASSERT_EQUAL_UINT(bufEntry, CheckDataBufHit(reqSlotTag));
	TEST_ASSERT_EQUAL_UINT(bufEntry, dataBufLruList.headEntry);
}

int main(void)
{
	UNITY_BEGIN();
	RUN_TEST(test_smoke_lookup_misses_on_empty_buffer);
	RUN_TEST(test_smoke_allocated_entry_hits_after_hash_insert);
	return UNITY_END();
}
