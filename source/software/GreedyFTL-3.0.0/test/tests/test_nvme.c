#include <string.h>
#include "test_support.h"
#include "nvme/nvme_identify.h"
#include "nvme/nvme_admin_cmd.h"
#include "nvme/nvme_io_cmd.h"

void setUp(void) { TestFtlReset(); }
void tearDown(void) {}

void test_identify_controller_reports_cosmos_identity(void)
{
	/* identify_* take a 32-bit DRAM address, so use scratch space inside the host DRAM image. */
	unsigned int bufferAddr = RESERVED_DATA_BUFFER_BASE_ADDR;
	ADMIN_IDENTIFY_CONTROLLER *id = (ADMIN_IDENTIFY_CONTROLLER *)bufferAddr;

	memset(id, 0xAA, 4096);
	identify_controller(bufferAddr);

	TEST_ASSERT_EQUAL_HEX16(PCI_VENDOR_ID, id->VID);
	TEST_ASSERT_EQUAL_MEMORY(MODEL_NUMBER, id->MN, strlen(MODEL_NUMBER));
	TEST_ASSERT_EQUAL_MEMORY(FIRMWARE_REVISION, id->FR, strlen(FIRMWARE_REVISION));
	TEST_ASSERT_EQUAL_UINT(1, id->NN);
}

void test_identify_namespace_reports_storage_capacity(void)
{
	unsigned int bufferAddr = RESERVED_DATA_BUFFER_BASE_ADDR;
	ADMIN_IDENTIFY_NAMESPACE *ns = (ADMIN_IDENTIFY_NAMESPACE *)bufferAddr;

	storageCapacity_L = 0x12345;
	memset(ns, 0xAA, 4096);
	identify_namespace(bufferAddr);

	TEST_ASSERT_EQUAL_UINT(0x12345, ns->NSZE[0]);
	TEST_ASSERT_EQUAL_UINT(0x12345, ns->NCAP[0]);
	TEST_ASSERT_EQUAL_UINT(0x12345, ns->NUSE[0]);
	TEST_ASSERT_EQUAL_UINT(12, ns->LBAFx[0].LBADS); /* 4 KiB LBAs */
}

static void MakeIoCmd(NVME_COMMAND *cmd, unsigned int opc, unsigned int slba, unsigned int nlb)
{
	NVME_IO_COMMAND *io = (NVME_IO_COMMAND *)cmd->cmdDword;
	memset(cmd, 0, sizeof(*cmd));
	cmd->cmdSlotTag = 5;
	io->OPC = opc;
	io->dword[10] = slba;
	io->dword[12] = nlb - 1;   /* NLB is zero-based */
}

void test_io_write_command_is_split_into_slice_requests_and_rx_dma(void)
{
	NVME_COMMAND cmd;

	storageCapacity_L = 1u << 20;
	MakeIoCmd(&cmd, IO_NVM_WRITE, 8 /* slice-aligned */, 2 * NVME_BLOCKS_PER_SLICE);
	handle_nvme_io_cmd(&cmd);

	TEST_ASSERT_EQUAL_UINT(2, sliceReqQ.reqCnt);
	TEST_ASSERT_EQUAL_UINT(8 / NVME_BLOCKS_PER_SLICE, reqPoolPtr->reqPool[sliceReqQ.headReq].logicalSliceAddr);
	TEST_ASSERT_EQUAL_UINT(REQ_CODE_WRITE, reqPoolPtr->reqPool[sliceReqQ.headReq].reqCode);
	TEST_ASSERT_EQUAL_UINT(5, reqPoolPtr->reqPool[sliceReqQ.headReq].nvmeCmdSlotTag);

	ReqTransSliceToLowLevel();
	TEST_ASSERT_EQUAL_UINT(0, sliceReqQ.reqCnt);
	TEST_ASSERT_EQUAL_UINT(2 * NVME_BLOCKS_PER_SLICE, FakeHostDmaCountFor(HOST_DMA_AUTO_TYPE, HOST_DMA_RX_DIRECTION));
}

void test_unaligned_io_read_spans_extra_slice(void)
{
	NVME_COMMAND cmd;
	unsigned int first;

	storageCapacity_L = 1u << 20;
	MakeIoCmd(&cmd, IO_NVM_READ, NVME_BLOCKS_PER_SLICE - 1, 2); /* straddles slices 0 and 1 */
	handle_nvme_io_cmd(&cmd);

	TEST_ASSERT_EQUAL_UINT(2, sliceReqQ.reqCnt);
	first = sliceReqQ.headReq;
	TEST_ASSERT_EQUAL_UINT(REQ_CODE_READ, reqPoolPtr->reqPool[first].reqCode);
	TEST_ASSERT_EQUAL_UINT(0, reqPoolPtr->reqPool[first].logicalSliceAddr);
	TEST_ASSERT_EQUAL_UINT(NVME_BLOCKS_PER_SLICE - 1, reqPoolPtr->reqPool[first].nvmeDmaInfo.nvmeBlockOffset);
	TEST_ASSERT_EQUAL_UINT(1, reqPoolPtr->reqPool[first].nvmeDmaInfo.numOfNvmeBlock);
	TEST_ASSERT_EQUAL_UINT(1, reqPoolPtr->reqPool[reqPoolPtr->reqPool[first].nextReq].logicalSliceAddr);
	TEST_ASSERT_EQUAL_UINT(1, reqPoolPtr->reqPool[reqPoolPtr->reqPool[first].nextReq].nvmeDmaInfo.startIndex);
}

void test_io_flush_completes_immediately_via_completion_fifo(void)
{
	NVME_COMMAND cmd;

	MakeIoCmd(&cmd, IO_NVM_FLUSH, 0, 1);
	handle_nvme_io_cmd(&cmd);

	TEST_ASSERT_EQUAL_UINT(0, sliceReqQ.reqCnt);
	TEST_ASSERT_EQUAL_UINT(1, FakeRegsWriteCountTo(NVME_CPL_FIFO_REG_ADDR + 8));
}

int main(void)
{
	UNITY_BEGIN();
	RUN_TEST(test_identify_controller_reports_cosmos_identity);
	RUN_TEST(test_identify_namespace_reports_storage_capacity);
	RUN_TEST(test_io_write_command_is_split_into_slice_requests_and_rx_dma);
	RUN_TEST(test_unaligned_io_read_spans_extra_slice);
	RUN_TEST(test_io_flush_completes_immediately_via_completion_fifo);
	return UNITY_END();
}
