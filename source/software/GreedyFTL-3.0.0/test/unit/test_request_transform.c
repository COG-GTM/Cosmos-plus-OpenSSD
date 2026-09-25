/* Smoke test for request_transform.c: an NVMe read spanning two slices is
 * split into two slice requests on the slice queue. */
#include "unity.h"
#include "ftl_test_env.h"
#include "request_allocation.h"
#include "request_transform.h"
#include "request_format.h"
#include "nvme/nvme.h"

void setUp(void) { ftl_test_env_reset(); ftl_test_env_init_ftl(); }
void tearDown(void) {}

static void test_read_spanning_two_slices_yields_two_slice_reqs(void)
{
	unsigned int first;

	TEST_ASSERT_EQUAL_UINT(0, sliceReqQ.reqCnt);
	ReqTransNvmeToSlice(3, NVME_BLOCKS_PER_SLICE - 1, 1, IO_NVM_READ);
	TEST_ASSERT_EQUAL_UINT(2, sliceReqQ.reqCnt);

	first = sliceReqQ.headReq;
	TEST_ASSERT_EQUAL_UINT(REQ_TYPE_SLICE, reqPoolPtr->reqPool[first].reqType);
	TEST_ASSERT_EQUAL_UINT(REQ_CODE_READ, reqPoolPtr->reqPool[first].reqCode);
	TEST_ASSERT_EQUAL_UINT(3, reqPoolPtr->reqPool[first].nvmeCmdSlotTag);
	TEST_ASSERT_EQUAL_UINT(0, reqPoolPtr->reqPool[first].logicalSliceAddr);
	TEST_ASSERT_EQUAL_UINT(1, reqPoolPtr->reqPool[sliceReqQ.tailReq].logicalSliceAddr);
}

int main(void)
{
	UNITY_BEGIN();
	RUN_TEST(test_read_spanning_two_slices_yields_two_slice_reqs);
	return UNITY_END();
}
