/* Smoke test for nvme_io_cmd.c + nvme_main.c: an NVMe read command is
 * decoded into slice requests via ReqTransNvmeToSlice. */
#include <string.h>
#include "unity.h"
#include "ftl_test_env.h"
#include "request_allocation.h"
#include "request_format.h"
#include "nvme/nvme.h"
#include "nvme/nvme_io_cmd.h"

void setUp(void) { ftl_test_env_reset(); ftl_test_env_init_ftl(); }
void tearDown(void) {}

static void test_io_read_decodes_into_slice_requests(void)
{
	NVME_COMMAND nvmeCmd;
	NVME_IO_COMMAND *io = (NVME_IO_COMMAND *)nvmeCmd.cmdDword;

	memset(&nvmeCmd, 0, sizeof(nvmeCmd));
	nvmeCmd.qID = 1;
	nvmeCmd.cmdSlotTag = 7;
	io->OPC = IO_NVM_READ;
	io->NSID = 1;
	io->PRP1[0] = 0x20000000;
	io->dword10 = 0;              /* start LBA */
	io->dword12 = NVME_BLOCKS_PER_SLICE * 2 - 1; /* NLB (0-based): two slices */

	handle_nvme_io_cmd(&nvmeCmd);

	TEST_ASSERT_EQUAL_UINT(2, sliceReqQ.reqCnt);
	TEST_ASSERT_EQUAL_UINT(7, reqPoolPtr->reqPool[sliceReqQ.headReq].nvmeCmdSlotTag);
	TEST_ASSERT_EQUAL_UINT(REQ_CODE_READ, reqPoolPtr->reqPool[sliceReqQ.headReq].reqCode);
}

int main(void)
{
	UNITY_BEGIN();
	RUN_TEST(test_io_read_decodes_into_slice_requests);
	return UNITY_END();
}
