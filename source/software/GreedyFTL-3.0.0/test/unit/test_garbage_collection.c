/* Unit tests for garbage_collection.c. */
#include "unity.h"

#include "fw_test.h"
#include "memory_map.h"

void setUp(void)
{
	fw_test_reset();
	fw_test_init_ftl();
}

void tearDown(void) {}

static void test_smoke_victim_list_returns_queued_block(void)
{
	const unsigned int dieNo = 0;
	const unsigned int blockNo = 7;

	PutToGcVictimList(dieNo, blockNo, 3);

	TEST_ASSERT_EQUAL_UINT(blockNo, GetFromGcVictimList(dieNo));
}

static void test_smoke_empty_victim_list_asserts(void)
{
	FW_EXPECT_ASSERT(GetFromGcVictimList(0));
}

int main(void)
{
	UNITY_BEGIN();
	RUN_TEST(test_smoke_victim_list_returns_queued_block);
	RUN_TEST(test_smoke_empty_victim_list_asserts);
	return UNITY_END();
}
