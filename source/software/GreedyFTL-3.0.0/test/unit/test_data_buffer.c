/* Smoke test for data_buffer.c: a freshly initialised buffer pool allocates
 * from the LRU tail and moves the entry to the head. */
#include "unity.h"
#include "ftl_test_env.h"
#include "data_buffer.h"

void setUp(void) { ftl_test_env_reset(); ftl_test_env_init_ftl(); }
void tearDown(void) {}

static void test_allocate_takes_lru_tail_and_promotes_to_head(void)
{
	unsigned int tail_before = dataBufLruList.tailEntry;
	unsigned int entry;

	TEST_ASSERT_NOT_EQUAL(DATA_BUF_NONE, tail_before);
	entry = AllocateDataBuf();
	TEST_ASSERT_EQUAL_UINT(tail_before, entry);
	TEST_ASSERT_EQUAL_UINT(entry, dataBufLruList.headEntry);
	TEST_ASSERT_NOT_EQUAL(entry, dataBufLruList.tailEntry);
	TEST_ASSERT_EQUAL_UINT(DATA_BUF_NONE, dataBufMapPtr->dataBuf[entry].prevEntry);
}

int main(void)
{
	UNITY_BEGIN();
	RUN_TEST(test_allocate_takes_lru_tail_and_promotes_to_head);
	return UNITY_END();
}
