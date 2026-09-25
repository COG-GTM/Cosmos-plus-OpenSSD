/* Smoke test for address_translation.c: after InitFTL() the logical map is
 * empty and a fresh virtual slice can be allocated and resolved to a die. */
#include "unity.h"
#include "ftl_test_env.h"
#include "address_translation.h"

void setUp(void) { ftl_test_env_reset(); ftl_test_env_init_ftl(); }
void tearDown(void) {}

static void test_fresh_map_has_no_mapping_and_allocates_first_slice(void)
{
	unsigned int lsa = 0, vsa;

	TEST_ASSERT_EQUAL_HEX32(VSA_NONE, logicalSliceMapPtr->logicalSlice[lsa].virtualSliceAddr);

	vsa = FindFreeVirtualSlice();
	TEST_ASSERT_NOT_EQUAL(VSA_FAIL, vsa);
	TEST_ASSERT_TRUE(Vsa2VdieTranslation(vsa) < USER_DIES);
	TEST_ASSERT_TRUE(Vsa2VblockTranslation(vsa) < USER_BLOCKS_PER_DIE);

	logicalSliceMapPtr->logicalSlice[lsa].virtualSliceAddr = vsa;
	virtualSliceMapPtr->virtualSlice[vsa].logicalSliceAddr = lsa;
	TEST_ASSERT_EQUAL_UINT(vsa, AddrTransRead(lsa));
}

int main(void)
{
	UNITY_BEGIN();
	RUN_TEST(test_fresh_map_has_no_mapping_and_allocates_first_slice);
	return UNITY_END();
}
