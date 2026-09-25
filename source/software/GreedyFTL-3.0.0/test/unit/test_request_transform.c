/* Unit tests for request_transform.c. */
#include "unity.h"

#include "fw_test.h"
#include "memory_map.h"

void setUp(void)
{
	fw_test_reset();
	fw_test_init_ftl();
}

void tearDown(void) {}

static void test_smoke_single_slice_read_becomes_one_slice_request(void)
{
	/* nlb is zero based: 0 means one 4 KiB NVMe block. */
	ReqTransNvmeToSlice(0, 0, 0, IO_NVM_READ);

	TEST_ASSERT_EQUAL_UINT(1, sliceReqQ.reqCnt);
	TEST_ASSERT_EQUAL_UINT(REQ_CODE_READ, reqPoolPtr->reqPool[sliceReqQ.headReq].reqCode);
	TEST_ASSERT_EQUAL_UINT(0, reqPoolPtr->reqPool[sliceReqQ.headReq].logicalSliceAddr);
}

static void test_smoke_two_slice_write_is_split(void)
{
	ReqTransNvmeToSlice(0, 0, 2 * NVME_BLOCKS_PER_SLICE - 1, IO_NVM_WRITE);

	TEST_ASSERT_EQUAL_UINT(2, sliceReqQ.reqCnt);
}

int main(void)
{
	UNITY_BEGIN();
	RUN_TEST(test_smoke_single_slice_read_becomes_one_slice_request);
	RUN_TEST(test_smoke_two_slice_write_is_split);
	return UNITY_END();
}
