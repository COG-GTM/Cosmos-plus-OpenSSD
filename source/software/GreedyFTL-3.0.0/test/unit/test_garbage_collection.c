/* Smoke test for garbage_collection.c: the victim list picks the block with
 * the most invalid slices first and unlinks it. */
#include "unity.h"
#include "ftl_test_env.h"
#include "address_translation.h"
#include "garbage_collection.h"

void setUp(void) { ftl_test_env_reset(); ftl_test_env_init_ftl(); }
void tearDown(void) {}

static void test_victim_list_returns_most_invalid_block_first(void)
{
	unsigned int die = 0;
	unsigned int mostly_invalid = 10, partly_invalid = 11;

	PutToGcVictimList(die, partly_invalid, 2);
	PutToGcVictimList(die, mostly_invalid, SLICES_PER_BLOCK);

	TEST_ASSERT_EQUAL_UINT(mostly_invalid, GetFromGcVictimList(die));
	TEST_ASSERT_EQUAL_UINT(BLOCK_NONE, gcVictimMapPtr->gcVictimList[die][SLICES_PER_BLOCK].headBlock);
	TEST_ASSERT_EQUAL_UINT(partly_invalid, GetFromGcVictimList(die));
	TEST_ASSERT_EQUAL_UINT(BLOCK_NONE, gcVictimMapPtr->gcVictimList[die][2].headBlock);
}

int main(void)
{
	UNITY_BEGIN();
	RUN_TEST(test_victim_list_returns_most_invalid_block_first);
	return UNITY_END();
}
