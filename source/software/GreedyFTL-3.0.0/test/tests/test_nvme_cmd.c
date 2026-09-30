#include "test_support.h"

void setUp(void)
{
	test_ftl_init();
}

void tearDown(void)
{
}

static unsigned int sliceQueueLength(void)
{
	unsigned int tag = sliceReqQ.headReq, count = 0;

	while (tag != REQ_SLOT_TAG_NONE && count <= AVAILABLE_OUNTSTANDING_REQ_COUNT)
	{
		count++;
		tag = reqPoolPtr->reqPool[tag].nextReq;
	}
	return count;
}

static void assertSlice(unsigned int tag, unsigned int lsa, unsigned int offset, unsigned int nBlocks, unsigned int startIndex)
{
	TEST_ASSERT_EQUAL_UINT(REQ_TYPE_SLICE, reqPoolPtr->reqPool[tag].reqType);
	TEST_ASSERT_EQUAL_UINT(lsa, reqPoolPtr->reqPool[tag].logicalSliceAddr);
	TEST_ASSERT_EQUAL_UINT(offset, reqPoolPtr->reqPool[tag].nvmeDmaInfo.nvmeBlockOffset);
	TEST_ASSERT_EQUAL_UINT(nBlocks, reqPoolPtr->reqPool[tag].nvmeDmaInfo.numOfNvmeBlock);
	TEST_ASSERT_EQUAL_UINT(startIndex, reqPoolPtr->reqPool[tag].nvmeDmaInfo.startIndex);
}

/* ---- Command FIFO ---------------------------------------------------------- */

static void test_command_fifo_pops_one_command_per_poll(void)
{
	unsigned int cmdDword[16] = {0};
	unsigned short qID, cmdSlotTag;
	unsigned int cmdSeqNum, out[16];

	cmdDword[0] = IO_NVM_FLUSH;
	fake_nvme_push_command(1, 7, 3, cmdDword);

	TEST_ASSERT_EQUAL_UINT(1, get_nvme_cmd(&qID, &cmdSlotTag, &cmdSeqNum, out));
	TEST_ASSERT_EQUAL_UINT(1, qID);
	TEST_ASSERT_EQUAL_UINT(7, cmdSlotTag);
	TEST_ASSERT_EQUAL_UINT(3, cmdSeqNum);
	TEST_ASSERT_EQUAL_UINT(IO_NVM_FLUSH, out[0]);

	TEST_ASSERT_EQUAL_UINT(0, get_nvme_cmd(&qID, &cmdSlotTag, &cmdSeqNum, out));

	cmdDword[0] = IO_NVM_READ;
	fake_nvme_push_command(2, 8, 4, cmdDword);
	TEST_ASSERT_EQUAL_UINT(1, get_nvme_cmd(&qID, &cmdSlotTag, &cmdSeqNum, out));
	TEST_ASSERT_EQUAL_UINT(8, cmdSlotTag);
	TEST_ASSERT_EQUAL_UINT(IO_NVM_READ, out[0]);
	TEST_ASSERT_EQUAL_UINT(0, get_nvme_cmd(&qID, &cmdSlotTag, &cmdSeqNum, out));
}

/* ---- NVMe LBA range -> slice requests --------------------------------------- */

static void test_single_block_read_becomes_one_partial_slice(void)
{
	unsigned int tag;

	ReqTransNvmeToSlice(3, 5, 0, IO_NVM_READ);

	TEST_ASSERT_EQUAL_UINT(1, sliceQueueLength());
	tag = sliceReqQ.headReq;
	assertSlice(tag, 5 / NVME_BLOCKS_PER_SLICE, 5 % NVME_BLOCKS_PER_SLICE, 1, 0);
	TEST_ASSERT_EQUAL_UINT(REQ_CODE_READ, reqPoolPtr->reqPool[tag].reqCode);
	TEST_ASSERT_EQUAL_UINT(3, reqPoolPtr->reqPool[tag].nvmeCmdSlotTag);
}

static void test_aligned_full_slice_write_is_one_request(void)
{
	ReqTransNvmeToSlice(0, 2 * NVME_BLOCKS_PER_SLICE, NVME_BLOCKS_PER_SLICE - 1, IO_NVM_WRITE);

	TEST_ASSERT_EQUAL_UINT(1, sliceQueueLength());
	assertSlice(sliceReqQ.headReq, 2, 0, NVME_BLOCKS_PER_SLICE, 0);
	TEST_ASSERT_EQUAL_UINT(REQ_CODE_WRITE, reqPoolPtr->reqPool[sliceReqQ.headReq].reqCode);
}

static void test_unaligned_range_splits_into_head_body_tail(void)
{
	/* LBA 1..10 (NLB = 9): head (3 blocks), one full slice, tail (3 blocks) */
	unsigned int tag;

	ReqTransNvmeToSlice(0, 1, 2 * NVME_BLOCKS_PER_SLICE + 1, IO_NVM_WRITE);

	TEST_ASSERT_EQUAL_UINT(3, sliceQueueLength());
	tag = sliceReqQ.headReq;
	assertSlice(tag, 0, 1, NVME_BLOCKS_PER_SLICE - 1, 0);
	tag = reqPoolPtr->reqPool[tag].nextReq;
	assertSlice(tag, 1, 0, NVME_BLOCKS_PER_SLICE, NVME_BLOCKS_PER_SLICE - 1);
	tag = reqPoolPtr->reqPool[tag].nextReq;
	assertSlice(tag, 2, 0, 3, 2 * NVME_BLOCKS_PER_SLICE - 1);
	TEST_ASSERT_EQUAL_UINT(AVAILABLE_OUNTSTANDING_REQ_COUNT - 3, freeReqQ.reqCnt);
}

static void test_range_ending_on_slice_boundary_has_no_tail(void)
{
	unsigned int tag;

	ReqTransNvmeToSlice(0, 2, 2 * NVME_BLOCKS_PER_SLICE - 3, IO_NVM_READ);

	TEST_ASSERT_EQUAL_UINT(2, sliceQueueLength());
	tag = sliceReqQ.headReq;
	assertSlice(tag, 0, 2, NVME_BLOCKS_PER_SLICE - 2, 0);
	tag = reqPoolPtr->reqPool[tag].nextReq;
	assertSlice(tag, 1, 0, NVME_BLOCKS_PER_SLICE, NVME_BLOCKS_PER_SLICE - 2);
}

static void test_io_command_handler_dispatches_read_and_write(void)
{
	NVME_COMMAND cmd;
	NVME_IO_COMMAND *io = (NVME_IO_COMMAND *)cmd.cmdDword;

	memset(&cmd, 0, sizeof(cmd));
	cmd.cmdSlotTag = 7;
	io->OPC = IO_NVM_READ;
	io->dword10 = 8;
	io->dword12 = 3;	/* NLB = 3 -> 4 blocks */
	handle_nvme_io_cmd(&cmd);

	TEST_ASSERT_EQUAL_UINT(1, sliceQueueLength());
	assertSlice(sliceReqQ.headReq, 2, 0, 4, 0);
	TEST_ASSERT_EQUAL_UINT(REQ_CODE_READ, reqPoolPtr->reqPool[sliceReqQ.headReq].reqCode);
	TEST_ASSERT_EQUAL_UINT(7, reqPoolPtr->reqPool[sliceReqQ.headReq].nvmeCmdSlotTag);

	io->OPC = IO_NVM_WRITE;
	io->dword10 = 12;
	handle_nvme_io_cmd(&cmd);
	TEST_ASSERT_EQUAL_UINT(2, sliceQueueLength());
	TEST_ASSERT_EQUAL_UINT(REQ_CODE_WRITE, reqPoolPtr->reqPool[sliceReqQ.tailReq].reqCode);
}

static void test_flush_command_completes_immediately(void)
{
	NVME_COMMAND cmd;
	NVME_IO_COMMAND *io = (NVME_IO_COMMAND *)cmd.cmdDword;

	memset(&cmd, 0, sizeof(cmd));
	cmd.cmdSlotTag = 9;
	io->OPC = IO_NVM_FLUSH;
	handle_nvme_io_cmd(&cmd);

	TEST_ASSERT_EQUAL_UINT(0, sliceQueueLength());
	TEST_ASSERT_EQUAL_UINT(1, fake_reg_write_count_to(NVME_CPL_FIFO_REG_ADDR + 8));
}

/* ---- admin commands ---------------------------------------------------------- */

static void test_identify_controller_fills_buffer_and_issues_one_tx_dma(void)
{
	NVME_ADMIN_COMMAND cmd;
	NVME_COMPLETION cpl;
	const ADMIN_IDENTIFY_CONTROLLER *id = host_mem_ptr(ADMIN_CMD_DRAM_DATA_BUFFER);
	const FAKE_DMA_DESCRIPTOR *dma;

	memset(&cmd, 0, sizeof(cmd));
	memset(&cpl, 0xFF, sizeof(cpl));
	cmd.OPC = ADMIN_IDENTIFY;
	cmd.PRP1[0] = 0x12345000;
	cmd.PRP1[1] = 0x1;
	cmd.dword10 = 1;	/* CNS = 1: controller */

	handle_identify(&cmd, &cpl);

	TEST_ASSERT_EQUAL_HEX16(PCI_VENDOR_ID, id->VID);
	TEST_ASSERT_EQUAL_MEMORY(SERIAL_NUMBER, id->SN, sizeof(SERIAL_NUMBER) - 1);
	TEST_ASSERT_EQUAL_MEMORY(MODEL_NUMBER, id->MN, sizeof(MODEL_NUMBER) - 1);
	TEST_ASSERT_EQUAL_MEMORY(FIRMWARE_REVISION, id->FR, sizeof(FIRMWARE_REVISION) - 1);
	TEST_ASSERT_EQUAL_UINT(0, cpl.statusFieldWord);
	TEST_ASSERT_EQUAL_UINT(0, cpl.specific);

	TEST_ASSERT_EQUAL_UINT(1, fake_dma_descriptor_count());
	dma = fake_dma_descriptor_at(0);
	TEST_ASSERT_EQUAL_UINT(ADMIN_CMD_DRAM_DATA_BUFFER, dma->devAddr);
	TEST_ASSERT_EQUAL_HEX32(0x12345000, dma->pcieAddrL);
	TEST_ASSERT_EQUAL_HEX32(0x1, dma->pcieAddrH);
	TEST_ASSERT_EQUAL_UINT(0x1000, dma->dmaLen);
	TEST_ASSERT_EQUAL_UINT(HOST_DMA_TX_DIRECTION, dma->dmaDirection);
	TEST_ASSERT_EQUAL_UINT(HOST_DMA_DIRECT_TYPE, dma->dmaType);
}

static void test_identify_namespace_reports_ftl_capacity(void)
{
	NVME_ADMIN_COMMAND cmd;
	NVME_COMPLETION cpl;
	const ADMIN_IDENTIFY_NAMESPACE *ns = host_mem_ptr(ADMIN_CMD_DRAM_DATA_BUFFER);

	memset(&cmd, 0, sizeof(cmd));
	cmd.OPC = ADMIN_IDENTIFY;
	cmd.NSID = 1;
	cmd.PRP1[0] = 0x00400000;
	cmd.dword10 = 0;	/* CNS = 0: namespace */

	handle_identify(&cmd, &cpl);

	TEST_ASSERT_EQUAL_UINT(storageCapacity_L, ns->NSZE[0]);
	TEST_ASSERT_EQUAL_UINT(storageCapacity_L, ns->NCAP[0]);
	TEST_ASSERT_EQUAL_UINT(STORAGE_CAPACITY_H, ns->NSZE[1]);
	TEST_ASSERT_EQUAL_UINT(1, fake_dma_descriptor_count());
}

static void test_identify_with_unaligned_prp_uses_two_dma_chunks(void)
{
	NVME_ADMIN_COMMAND cmd;
	NVME_COMPLETION cpl;

	memset(&cmd, 0, sizeof(cmd));
	cmd.OPC = ADMIN_IDENTIFY;
	cmd.PRP1[0] = 0x00400800;	/* half-way into a 4 KiB page */
	cmd.PRP2[0] = 0x00500000;
	cmd.dword10 = 1;

	handle_identify(&cmd, &cpl);

	TEST_ASSERT_EQUAL_UINT(2, fake_dma_descriptor_count());
	TEST_ASSERT_EQUAL_UINT(0x800, fake_dma_descriptor_at(0)->dmaLen);
	TEST_ASSERT_EQUAL_HEX32(0x00400800, fake_dma_descriptor_at(0)->pcieAddrL);
	TEST_ASSERT_EQUAL_UINT(0x800, fake_dma_descriptor_at(1)->dmaLen);
	TEST_ASSERT_EQUAL_HEX32(0x00500000, fake_dma_descriptor_at(1)->pcieAddrL);
	TEST_ASSERT_EQUAL_UINT(ADMIN_CMD_DRAM_DATA_BUFFER + 0x800, fake_dma_descriptor_at(1)->devAddr);
}

static void test_set_number_of_queues_is_clamped_to_device_limit(void)
{
	NVME_ADMIN_COMMAND cmd;
	NVME_COMPLETION cpl;

	memset(&cmd, 0, sizeof(cmd));
	memset(&cpl, 0, sizeof(cpl));
	cmd.OPC = ADMIN_SET_FEATURES;
	cmd.dword10 = NUMBER_OF_QUEUES;
	cmd.dword11 = (15u << 16) | 15u;	/* host asks for 16 SQ / 16 CQ */
	handle_set_features(&cmd, &cpl);

	TEST_ASSERT_EQUAL_UINT(0, cpl.statusFieldWord);
	TEST_ASSERT_EQUAL_UINT(MAX_NUM_OF_IO_SQ - 1, cpl.specific & 0xFFFF);
	TEST_ASSERT_EQUAL_UINT(MAX_NUM_OF_IO_CQ - 1, cpl.specific >> 16);

	cmd.dword11 = (2u << 16) | 3u;		/* below the limit: passed through */
	handle_set_features(&cmd, &cpl);
	TEST_ASSERT_EQUAL_HEX32((2u << 16) | 3u, cpl.specific);
}

static void test_volatile_write_cache_feature_round_trips(void)
{
	NVME_ADMIN_COMMAND cmd;
	NVME_COMPLETION cpl;

	memset(&cmd, 0, sizeof(cmd));
	cmd.OPC = ADMIN_SET_FEATURES;
	cmd.dword10 = VOLATILE_WRITE_CACHE;
	cmd.dword11 = 1;
	handle_set_features(&cmd, &cpl);
	TEST_ASSERT_EQUAL_UINT(1, g_nvmeTask.cacheEn);

	cmd.OPC = ADMIN_GET_FEATURES;
	cmd.dword11 = 0;
	handle_get_features(&cmd, &cpl);
	TEST_ASSERT_EQUAL_UINT(0, cpl.statusFieldWord);
	TEST_ASSERT_EQUAL_UINT(1, cpl.specific);
}

static void test_create_io_queues_program_queue_registers(void)
{
	NVME_ADMIN_COMMAND cmd;
	NVME_COMPLETION cpl;
	unsigned int value;

	memset(&cmd, 0, sizeof(cmd));
	cmd.OPC = ADMIN_CREATE_IO_CQ;
	cmd.PRP1[0] = 0x00600000;
	cmd.dword10 = (63u << 16) | 1u;	/* QSIZE = 64, QID = 1 */
	cmd.dword11 = (1u << 16) | 0x3u;	/* IV = 1, IEN, PC */
	handle_create_io_cq(&cmd, &cpl);
	TEST_ASSERT_EQUAL_UINT(0, cpl.statusFieldWord);
	TEST_ASSERT_TRUE(fake_reg_last_write_to(NVME_IO_CQ_SET_REG_ADDR + 0 * 8 + 4, &value));
	TEST_ASSERT_TRUE(value & (1u << 15));	/* valid */
	TEST_ASSERT_TRUE(fake_reg_last_write_to(NVME_IO_CQ_SET_REG_ADDR + 0 * 8, &value));
	TEST_ASSERT_EQUAL_HEX32(0x00600000, value);

	cmd.OPC = ADMIN_CREATE_IO_SQ;
	cmd.PRP1[0] = 0x00700000;
	cmd.dword11 = (1u << 16) | 0x1u;	/* CQID = 1, PC */
	handle_create_io_sq(&cmd, &cpl);
	TEST_ASSERT_EQUAL_UINT(0, cpl.statusFieldWord);
	TEST_ASSERT_TRUE(fake_reg_last_write_to(NVME_IO_SQ_SET_REG_ADDR + 0 * 8, &value));
	TEST_ASSERT_EQUAL_HEX32(0x00700000, value);

	cmd.OPC = ADMIN_DELETE_IO_SQ;
	cmd.dword10 = 1;
	handle_delete_io_sq(&cmd, &cpl);
	TEST_ASSERT_TRUE(fake_reg_last_write_to(NVME_IO_SQ_SET_REG_ADDR + 0 * 8 + 4, &value));
	TEST_ASSERT_FALSE(value & (1u << 15));
}

int main(void)
{
	UNITY_BEGIN();
	RUN_TEST(test_command_fifo_pops_one_command_per_poll);
	RUN_TEST(test_single_block_read_becomes_one_partial_slice);
	RUN_TEST(test_aligned_full_slice_write_is_one_request);
	RUN_TEST(test_unaligned_range_splits_into_head_body_tail);
	RUN_TEST(test_range_ending_on_slice_boundary_has_no_tail);
	RUN_TEST(test_io_command_handler_dispatches_read_and_write);
	RUN_TEST(test_flush_command_completes_immediately);
	RUN_TEST(test_identify_controller_fills_buffer_and_issues_one_tx_dma);
	RUN_TEST(test_identify_namespace_reports_ftl_capacity);
	RUN_TEST(test_identify_with_unaligned_prp_uses_two_dma_chunks);
	RUN_TEST(test_set_number_of_queues_is_clamped_to_device_limit);
	RUN_TEST(test_volatile_write_cache_feature_round_trips);
	RUN_TEST(test_create_io_queues_program_queue_registers);
	return UNITY_END();
}
