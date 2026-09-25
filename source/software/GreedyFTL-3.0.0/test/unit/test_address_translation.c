/* Unit tests for address_translation.c. */
#include "unity.h"

#include "fw_test.h"
#include "memory_map.h"

void setUp(void)
{
	fw_test_reset();
	fw_test_init_ftl();
}

void tearDown(void) {}

static void test_smoke_unwritten_lsa_translates_to_vsa_fail(void)
{
	TEST_ASSERT_EQUAL_HEX32(VSA_FAIL, AddrTransRead(0));
}

static void test_smoke_write_then_read_maps_lsa_to_same_vsa(void)
{
	const unsigned int lsa = 5;
	unsigned int vsa = AddrTransWrite(lsa);

	TEST_ASSERT_NOT_EQUAL(VSA_FAIL, vsa);
	TEST_ASSERT_EQUAL_HEX32(vsa, AddrTransRead(lsa));
	TEST_ASSERT_EQUAL_UINT(lsa, virtualSliceMapPtr->virtualSlice[vsa].logicalSliceAddr);
}

int main(void)
{
	UNITY_BEGIN();
	RUN_TEST(test_smoke_unwritten_lsa_translates_to_vsa_fail);
	RUN_TEST(test_smoke_write_then_read_maps_lsa_to_same_vsa);
	return UNITY_END();
}
