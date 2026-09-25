#include <string.h>

#include "unity.h"

#include "ftl_test_env.h"
#include "nvme/nvme_admin_cmd.h"
#include "nvme/nvme_identify.h"
#include "nvme/nvme_io_cmd.h"

/* Defined in nvme_main.c without a header declaration. */
extern volatile NVME_CONTEXT g_nvmeTask;

#define ADMIN_SLOT 5u
#define IO_SLOT 9u
#define HOST_PRP_BASE 0x40000000u /* arbitrary 4 KiB aligned host address */

void setUp(void)
{
	ftl_test_env_init();
	memset((void *)&g_nvmeTask, 0, sizeof(g_nvmeTask));
}

void tearDown(void)
{
}

static NVME_COMMAND MakeAdminCmd(unsigned char opc)
{
	NVME_COMMAND cmd;
	NVME_ADMIN_COMMAND *admin = (NVME_ADMIN_COMMAND *)cmd.cmdDword;

	memset(&cmd, 0, sizeof(cmd));
	cmd.qID = 0;
	cmd.cmdSlotTag = ADMIN_SLOT;
	admin->OPC = opc;
	admin->NSID = 1;
	admin->PRP1[0] = HOST_PRP_BASE;
	admin->PRP2[0] = HOST_PRP_BASE + 0x1000;
	return cmd;
}

static NVME_COMMAND MakeIoCmd(unsigned char opc, unsigned int startLba, unsigned int nlb0)
{
	NVME_COMMAND cmd;
	NVME_IO_COMMAND *io = (NVME_IO_COMMAND *)cmd.cmdDword;

	memset(&cmd, 0, sizeof(cmd));
	cmd.qID = 1;
	cmd.cmdSlotTag = IO_SLOT;
	io->OPC = opc;
	io->NSID = 1;
	io->PRP1[0] = HOST_PRP_BASE;
	io->PRP2[0] = HOST_PRP_BASE + 0x1000;
	io->dword[10] = startLba;
	io->dword[11] = 0;
	io->dword[12] = nlb0;
	return cmd;
}

/* Buffered writes are only mapped to NAND on eviction, so a fresh write is
 * visible as a dirty data-buffer entry rather than through AddrTransRead(). */
static int IsBufferedDirty(unsigned int lsa)
{
	unsigned int entry;

	for (entry = 0; entry < AVAILABLE_DATA_BUFFER_ENTRY_COUNT; entry++)
		if (dataBufMapPtr->dataBuf[entry].logicalSliceAddr == lsa)
			return dataBufMapPtr->dataBuf[entry].dirty == DATA_BUF_DIRTY;
	return 0;
}

static FAKE_NVME_CPL OnlyCpl(void)
{
	TEST_ASSERT_EQUAL_UINT32(1, fake_nvme_cpl_count());
	return fake_nvme_cpl_at(0);
}

/* ---- Identify ------------------------------------------------------------ */

static void test_identify_controller_fills_buffer_and_dmas_it_to_prp1(void)
{
	NVME_COMMAND cmd = MakeAdminCmd(ADMIN_IDENTIFY);
	ADMIN_IDENTIFY_CONTROLLER *ctrl = host_memory_ptr(ADMIN_CMD_DRAM_DATA_BUFFER);
	FAKE_DMA_DESC dma;
	FAKE_NVME_CPL cpl;

	cmd.cmdDword[10] = 1; /* CNS = 1: controller */

	handle_nvme_admin_cmd(&cmd);

	TEST_ASSERT_EQUAL_HEX16(PCI_VENDOR_ID, ctrl->VID);
	TEST_ASSERT_EQUAL_MEMORY(SERIAL_NUMBER, ctrl->SN, sizeof(SERIAL_NUMBER) - 1);
	TEST_ASSERT_EQUAL_MEMORY(MODEL_NUMBER, ctrl->MN, sizeof(MODEL_NUMBER) - 1);
	TEST_ASSERT_EQUAL_UINT32(1, ctrl->NN);
	TEST_ASSERT_EQUAL_UINT8(0x6, ctrl->SQES.requiredSubmissionQueueEntrySize);

	TEST_ASSERT_EQUAL_UINT32(1, fake_dma_count());
	dma = fake_dma_at(0);
	TEST_ASSERT_EQUAL_UINT32(HOST_DMA_DIRECT_TYPE, dma.dmaType);
	TEST_ASSERT_EQUAL_UINT32(HOST_DMA_TX_DIRECTION, dma.dmaDirection);
	TEST_ASSERT_EQUAL_HEX32(ADMIN_CMD_DRAM_DATA_BUFFER, dma.devAddr);
	TEST_ASSERT_EQUAL_HEX32(HOST_PRP_BASE, dma.pcieAddrL);
	TEST_ASSERT_EQUAL_UINT32(0x1000, dma.len);

	cpl = OnlyCpl();
	TEST_ASSERT_EQUAL_UINT32(AUTO_CPL_TYPE, cpl.cplType);
	TEST_ASSERT_EQUAL_UINT32(ADMIN_SLOT, cpl.cmdSlotTag);
	TEST_ASSERT_EQUAL_UINT32(0, cpl.specific);
	TEST_ASSERT_EQUAL_UINT32(0, cpl.statusFieldWord);
}

static void test_identify_namespace_reports_capacity_and_4k_lba_format(void)
{
	NVME_COMMAND cmd = MakeAdminCmd(ADMIN_IDENTIFY);
	ADMIN_IDENTIFY_NAMESPACE *ns = host_memory_ptr(ADMIN_CMD_DRAM_DATA_BUFFER);

	cmd.cmdDword[10] = 0; /* CNS = 0: namespace */

	handle_nvme_admin_cmd(&cmd);

	TEST_ASSERT_EQUAL_UINT32(storageCapacity_L, ns->NSZE[0]);
	TEST_ASSERT_EQUAL_UINT32(storageCapacity_L, ns->NCAP[0]);
	TEST_ASSERT_EQUAL_UINT32(storageCapacity_L, ns->NUSE[0]);
	TEST_ASSERT_EQUAL_UINT8(0, ns->NLBAF);
	TEST_ASSERT_EQUAL_UINT8(0xC, ns->LBAFx[0].LBADS); /* 2^12 = 4 KiB */
	TEST_ASSERT_EQUAL_UINT32(1, fake_dma_count());
	TEST_ASSERT_EQUAL_UINT32(AUTO_CPL_TYPE, OnlyCpl().cplType);
}

static void test_identify_with_unaligned_prp1_splits_transfer_across_prp2(void)
{
	NVME_COMMAND cmd = MakeAdminCmd(ADMIN_IDENTIFY);
	NVME_ADMIN_COMMAND *admin = (NVME_ADMIN_COMMAND *)cmd.cmdDword;
	FAKE_DMA_DESC first, second;

	cmd.cmdDword[10] = 1;
	admin->PRP1[0] = HOST_PRP_BASE + 0x800;

	handle_nvme_admin_cmd(&cmd);

	TEST_ASSERT_EQUAL_UINT32(2, fake_dma_count());
	first = fake_dma_at(0);
	second = fake_dma_at(1);
	TEST_ASSERT_EQUAL_UINT32(0x800, first.len);
	TEST_ASSERT_EQUAL_HEX32(HOST_PRP_BASE + 0x800, first.pcieAddrL);
	TEST_ASSERT_EQUAL_UINT32(0x800, second.len);
	TEST_ASSERT_EQUAL_HEX32(ADMIN_CMD_DRAM_DATA_BUFFER + 0x800, second.devAddr);
	TEST_ASSERT_EQUAL_HEX32(HOST_PRP_BASE + 0x1000, second.pcieAddrL);
}

/* ---- Set / Get features -------------------------------------------------- */

static void test_set_features_number_of_queues_clamps_to_supported_maximum(void)
{
	NVME_COMMAND cmd = MakeAdminCmd(ADMIN_SET_FEATURES);
	ADMIN_SET_FEATURES_NUMBER_OF_QUEUES_DW11 result;

	cmd.cmdDword[10] = NUMBER_OF_QUEUES;
	cmd.cmdDword[11] = (0x3F << 16) | 0x3F; /* host asks for 64 SQs and 64 CQs */

	handle_nvme_admin_cmd(&cmd);

	result.dword = OnlyCpl().specific;
	TEST_ASSERT_EQUAL_UINT32(MAX_NUM_OF_IO_SQ - 1, result.NSQR);
	TEST_ASSERT_EQUAL_UINT32(MAX_NUM_OF_IO_CQ - 1, result.NCQR);
}

static void test_set_features_number_of_queues_passes_small_requests_through(void)
{
	NVME_COMMAND cmd = MakeAdminCmd(ADMIN_SET_FEATURES);

	cmd.cmdDword[10] = NUMBER_OF_QUEUES;
	cmd.cmdDword[11] = (2 << 16) | 3;

	handle_nvme_admin_cmd(&cmd);

	TEST_ASSERT_EQUAL_HEX32((2 << 16) | 3, OnlyCpl().specific);
}

static void test_set_features_volatile_write_cache_toggles_cache_enable(void)
{
	NVME_COMMAND cmd = MakeAdminCmd(ADMIN_SET_FEATURES);

	cmd.cmdDword[10] = VOLATILE_WRITE_CACHE;
	cmd.cmdDword[11] = 1;
	handle_nvme_admin_cmd(&cmd);
	TEST_ASSERT_EQUAL_UINT32(1, g_nvmeTask.cacheEn);

	cmd.cmdDword[11] = 0;
	handle_nvme_admin_cmd(&cmd);
	TEST_ASSERT_EQUAL_UINT32(0, g_nvmeTask.cacheEn);

	TEST_ASSERT_EQUAL_UINT32(2, fake_nvme_cpl_count());
	TEST_ASSERT_EQUAL_UINT32(0, fake_nvme_cpl_at(1).specific);
}

static void test_get_features_volatile_write_cache_reports_current_setting(void)
{
	NVME_COMMAND cmd = MakeAdminCmd(ADMIN_GET_FEATURES);

	g_nvmeTask.cacheEn = 1;
	cmd.cmdDword[10] = VOLATILE_WRITE_CACHE;
	handle_nvme_admin_cmd(&cmd);

	TEST_ASSERT_EQUAL_UINT32(1, OnlyCpl().specific);
	TEST_ASSERT_EQUAL_UINT32(0, fake_dma_count());
}

static void test_get_features_temperature_threshold_echoes_dword11(void)
{
	NVME_COMMAND cmd = MakeAdminCmd(ADMIN_GET_FEATURES);

	cmd.cmdDword[10] = TEMPERATURE_THRESHOLD;
	cmd.cmdDword[11] = 0x1234;
	handle_nvme_admin_cmd(&cmd);

	TEST_ASSERT_EQUAL_HEX32(0x1234, OnlyCpl().specific);
}

static void test_get_features_lba_range_type_fails_with_invalid_field(void)
{
	NVME_COMMAND cmd = MakeAdminCmd(ADMIN_GET_FEATURES);
	NVME_COMPLETION cpl;

	cmd.cmdDword[10] = LBA_RANGE_TYPE;
	handle_nvme_admin_cmd(&cmd);

	cpl.statusFieldWord = OnlyCpl().statusFieldWord;
	TEST_ASSERT_EQUAL_UINT32(SC_INVALID_FIELD_IN_COMMAND, cpl.statusField.SC);
}

/* ---- Queue management ---------------------------------------------------- */

static void test_create_io_cq_records_state_and_programs_cq_register(void)
{
	NVME_COMMAND cmd = MakeAdminCmd(ADMIN_CREATE_IO_CQ);
	NVME_IO_CQ_SET_REG reg;
	unsigned int qid = 2, qsize = 0x3F, iv = 3;

	cmd.cmdDword[10] = (qsize << 16) | qid;
	cmd.cmdDword[11] = (iv << 16) | (1u << 1) | 1u; /* IEN=1, PC=1 */

	handle_nvme_admin_cmd(&cmd);

	TEST_ASSERT_EQUAL_UINT32(1, g_nvmeTask.ioCqInfo[qid - 1].valid);
	TEST_ASSERT_EQUAL_UINT32(qsize, g_nvmeTask.ioCqInfo[qid - 1].qSzie);
	TEST_ASSERT_EQUAL_UINT32(1, g_nvmeTask.ioCqInfo[qid - 1].irqEn);
	TEST_ASSERT_EQUAL_UINT32(iv, g_nvmeTask.ioCqInfo[qid - 1].irqVector);
	TEST_ASSERT_EQUAL_HEX32(HOST_PRP_BASE, g_nvmeTask.ioCqInfo[qid - 1].pcieBaseAddrL);

	TEST_ASSERT_EQUAL_UINT32(1, fake_reg_writes_to(NVME_IO_CQ_SET_REG_ADDR + (qid - 1) * 8, &reg.dword[0]));
	TEST_ASSERT_EQUAL_UINT32(1, fake_reg_writes_to(NVME_IO_CQ_SET_REG_ADDR + (qid - 1) * 8 + 4, &reg.dword[1]));
	TEST_ASSERT_EQUAL_HEX32(HOST_PRP_BASE, reg.pcieBaseAddrL);
	TEST_ASSERT_EQUAL_UINT32(1, reg.valid);
	TEST_ASSERT_EQUAL_UINT32(1, reg.irqEn);
	TEST_ASSERT_EQUAL_UINT32(iv, reg.irqVector);
	TEST_ASSERT_EQUAL_UINT32(qsize, reg.cqSize);
	TEST_ASSERT_EQUAL_UINT32(0, OnlyCpl().specific);
}

static void test_create_then_delete_io_sq_programs_and_clears_sq_register(void)
{
	NVME_COMMAND cmd = MakeAdminCmd(ADMIN_CREATE_IO_SQ);
	NVME_IO_SQ_SET_REG reg;
	unsigned int qid = 1, qsize = 0x7F, cqid = 4;

	cmd.cmdDword[10] = (qsize << 16) | qid;
	cmd.cmdDword[11] = (cqid << 16) | 1u;
	handle_nvme_admin_cmd(&cmd);

	TEST_ASSERT_EQUAL_UINT32(1, g_nvmeTask.ioSqInfo[qid - 1].valid);
	TEST_ASSERT_EQUAL_UINT32(cqid, g_nvmeTask.ioSqInfo[qid - 1].cqVector);
	fake_reg_writes_to(NVME_IO_SQ_SET_REG_ADDR + (qid - 1) * 8, &reg.dword[0]);
	fake_reg_writes_to(NVME_IO_SQ_SET_REG_ADDR + (qid - 1) * 8 + 4, &reg.dword[1]);
	TEST_ASSERT_EQUAL_UINT32(1, reg.valid);
	TEST_ASSERT_EQUAL_UINT32(cqid, reg.cqVector);
	TEST_ASSERT_EQUAL_UINT32(qsize, reg.sqSize);
	TEST_ASSERT_EQUAL_HEX32(HOST_PRP_BASE, reg.pcieBaseAddrL);

	cmd = MakeAdminCmd(ADMIN_DELETE_IO_SQ);
	cmd.cmdDword[10] = qid;
	handle_nvme_admin_cmd(&cmd);

	TEST_ASSERT_EQUAL_UINT32(0, g_nvmeTask.ioSqInfo[qid - 1].valid);
	TEST_ASSERT_EQUAL_UINT32(2, fake_reg_writes_to(NVME_IO_SQ_SET_REG_ADDR + (qid - 1) * 8, &reg.dword[0]));
	TEST_ASSERT_EQUAL_UINT32(2, fake_reg_writes_to(NVME_IO_SQ_SET_REG_ADDR + (qid - 1) * 8 + 4, &reg.dword[1]));
	TEST_ASSERT_EQUAL_HEX32(0, reg.dword[0]);
	TEST_ASSERT_EQUAL_HEX32(0, reg.dword[1]);
	TEST_ASSERT_EQUAL_UINT32(2, fake_nvme_cpl_count());
}

static void test_delete_io_cq_clears_state_and_register(void)
{
	NVME_COMMAND cmd = MakeAdminCmd(ADMIN_DELETE_IO_CQ);
	unsigned int qid = 3, value = 0xffffffffu;

	g_nvmeTask.ioCqInfo[qid - 1].valid = 1;
	cmd.cmdDword[10] = qid;

	handle_nvme_admin_cmd(&cmd);

	TEST_ASSERT_EQUAL_UINT32(0, g_nvmeTask.ioCqInfo[qid - 1].valid);
	TEST_ASSERT_EQUAL_UINT32(1, fake_reg_writes_to(NVME_IO_CQ_SET_REG_ADDR + (qid - 1) * 8 + 4, &value));
	TEST_ASSERT_EQUAL_HEX32(0, value);
}

/* ---- Completion plumbing ------------------------------------------------- */

static void test_async_event_request_releases_slot_without_completion(void)
{
	NVME_COMMAND cmd = MakeAdminCmd(ADMIN_ASYNCHRONOUS_EVENT_REQUEST);
	FAKE_NVME_CPL cpl;

	handle_nvme_admin_cmd(&cmd);

	cpl = OnlyCpl();
	TEST_ASSERT_EQUAL_UINT32(CMD_SLOT_RELEASE_TYPE, cpl.cplType);
	TEST_ASSERT_EQUAL_UINT32(ADMIN_SLOT, cpl.cmdSlotTag);
}

static void test_get_log_page_reports_invalid_log_page(void)
{
	NVME_COMMAND cmd = MakeAdminCmd(ADMIN_GET_LOG_PAGE);

	handle_nvme_admin_cmd(&cmd);

	TEST_ASSERT_EQUAL_HEX32(0x9, OnlyCpl().specific);
}

static void test_completion_register_writes_carry_slot_tag_and_cpl_type(void)
{
	unsigned int dword2 = 0;
	NVME_CPL_FIFO_REG reg;

	set_auto_nvme_cpl(IO_SLOT, 0xabcd, 0);

	TEST_ASSERT_EQUAL_UINT32(1, fake_reg_writes_to(NVME_CPL_FIFO_REG_ADDR + 8, &dword2));
	reg.dword[2] = dword2;
	TEST_ASSERT_EQUAL_UINT32(IO_SLOT, reg.cmdSlotTag);
	TEST_ASSERT_EQUAL_UINT32(AUTO_CPL_TYPE, reg.cplType);
	/* Auto completions carry `specific` in dword[1]; dword[0] (sqId/cid) is
	 * left to the hardware and must not be written. */
	TEST_ASSERT_EQUAL_UINT32(1, fake_reg_writes_to(NVME_CPL_FIFO_REG_ADDR + 4, &dword2));
	TEST_ASSERT_EQUAL_HEX32(0xabcd, dword2);
	TEST_ASSERT_EQUAL_UINT32(0, fake_reg_writes_to(NVME_CPL_FIFO_REG_ADDR, &dword2));
}

/* ---- I/O commands -------------------------------------------------------- */

static void test_flush_completes_immediately_without_touching_ftl(void)
{
	NVME_COMMAND cmd = MakeIoCmd(IO_NVM_FLUSH, 0, 0);
	FAKE_NVME_CPL cpl;

	handle_nvme_io_cmd(&cmd);

	cpl = OnlyCpl();
	TEST_ASSERT_EQUAL_UINT32(AUTO_CPL_TYPE, cpl.cplType);
	TEST_ASSERT_EQUAL_UINT32(IO_SLOT, cpl.cmdSlotTag);
	TEST_ASSERT_EQUAL_UINT32(0, sliceReqQ.reqCnt);
	TEST_ASSERT_EQUAL_UINT32(0, fake_dma_count());
}

static void test_write_command_creates_slice_requests_and_rx_dmas(void)
{
	unsigned int startLba = 8 * NVME_BLOCKS_PER_SLICE;
	unsigned int nlb = 2 * NVME_BLOCKS_PER_SLICE; /* two whole slices */
	NVME_COMMAND cmd = MakeIoCmd(IO_NVM_WRITE, startLba, nlb - 1);
	unsigned int i, rx;

	handle_nvme_io_cmd(&cmd);
	TEST_ASSERT_EQUAL_UINT32(2, sliceReqQ.reqCnt);

	ReqTransSliceToLowLevel();
	ftl_test_drain();

	TEST_ASSERT_EQUAL_UINT32(0, sliceReqQ.reqCnt);
	rx = fake_dma_count_of(HOST_DMA_AUTO_TYPE, HOST_DMA_RX_DIRECTION);
	TEST_ASSERT_EQUAL_UINT32(nlb, rx);
	for (i = 0; i < fake_dma_count(); i++)
		TEST_ASSERT_EQUAL_UINT32(IO_SLOT, fake_dma_at(i).cmdSlotTag);

	TEST_ASSERT_TRUE(IsBufferedDirty(8));
	TEST_ASSERT_TRUE(IsBufferedDirty(9));
	TEST_ASSERT_FALSE(IsBufferedDirty(10));
	TEST_ASSERT_EQUAL_UINT32(0, fake_dma_count_of(HOST_DMA_AUTO_TYPE, HOST_DMA_TX_DIRECTION));
}

static void test_read_command_streams_tx_dmas_with_4k_offsets(void)
{
	unsigned int lsa = 20;
	NVME_COMMAND cmd = MakeIoCmd(IO_NVM_READ, lsa * NVME_BLOCKS_PER_SLICE, NVME_BLOCKS_PER_SLICE - 1);
	unsigned int i, tx;

	ftl_test_write_slice(lsa, 0x5a);

	handle_nvme_io_cmd(&cmd);
	ReqTransSliceToLowLevel();
	ftl_test_drain();

	tx = fake_dma_count_of(HOST_DMA_AUTO_TYPE, HOST_DMA_TX_DIRECTION);
	TEST_ASSERT_EQUAL_UINT32(NVME_BLOCKS_PER_SLICE, tx);
	for (i = 0; i < fake_dma_count(); i++)
	{
		FAKE_DMA_DESC dma = fake_dma_at(i);
		TEST_ASSERT_EQUAL_UINT32(IO_SLOT, dma.cmdSlotTag);
		TEST_ASSERT_EQUAL_UINT32(i, dma.cmd4KBOffset);
	}
	TEST_ASSERT_EQUAL_UINT32(0, fake_dma_count_of(HOST_DMA_AUTO_TYPE, HOST_DMA_RX_DIRECTION));
}

static void test_partial_write_of_unwritten_slice_skips_nand_read(void)
{
	NVME_COMMAND cmd = MakeIoCmd(IO_NVM_WRITE, 30 * NVME_BLOCKS_PER_SLICE + 1, 0); /* one 4 KiB block */
	unsigned int readsBefore = 0, ch, way;

	for (ch = 0; ch < USER_CHANNELS; ch++)
		for (way = 0; way < USER_WAYS; way++)
			readsBefore += fake_nand_stats(ch, way).readTriggers;

	handle_nvme_io_cmd(&cmd);
	ReqTransSliceToLowLevel();
	ftl_test_drain();

	TEST_ASSERT_EQUAL_UINT32(1, fake_dma_count_of(HOST_DMA_AUTO_TYPE, HOST_DMA_RX_DIRECTION));
	TEST_ASSERT_TRUE(IsBufferedDirty(30));

	for (ch = 0; ch < USER_CHANNELS; ch++)
		for (way = 0; way < USER_WAYS; way++)
			readsBefore -= fake_nand_stats(ch, way).readTriggers;
	TEST_ASSERT_EQUAL_UINT32(0, readsBefore);
}

static void test_write_spanning_slice_boundary_touches_both_slices(void)
{
	/* Starts on the last block of slice 40 and ends on the first block of slice 41. */
	NVME_COMMAND cmd = MakeIoCmd(IO_NVM_WRITE, 41 * NVME_BLOCKS_PER_SLICE - 1, 1);

	handle_nvme_io_cmd(&cmd);
	TEST_ASSERT_EQUAL_UINT32(2, sliceReqQ.reqCnt);

	ReqTransSliceToLowLevel();
	ftl_test_drain();

	TEST_ASSERT_TRUE(IsBufferedDirty(40));
	TEST_ASSERT_TRUE(IsBufferedDirty(41));
	TEST_ASSERT_EQUAL_UINT32(2, fake_dma_count_of(HOST_DMA_AUTO_TYPE, HOST_DMA_RX_DIRECTION));
}

int main(void)
{
	UNITY_BEGIN();
	RUN_TEST(test_identify_controller_fills_buffer_and_dmas_it_to_prp1);
	RUN_TEST(test_identify_namespace_reports_capacity_and_4k_lba_format);
	RUN_TEST(test_identify_with_unaligned_prp1_splits_transfer_across_prp2);
	RUN_TEST(test_set_features_number_of_queues_clamps_to_supported_maximum);
	RUN_TEST(test_set_features_number_of_queues_passes_small_requests_through);
	RUN_TEST(test_set_features_volatile_write_cache_toggles_cache_enable);
	RUN_TEST(test_get_features_volatile_write_cache_reports_current_setting);
	RUN_TEST(test_get_features_temperature_threshold_echoes_dword11);
	RUN_TEST(test_get_features_lba_range_type_fails_with_invalid_field);
	RUN_TEST(test_create_io_cq_records_state_and_programs_cq_register);
	RUN_TEST(test_create_then_delete_io_sq_programs_and_clears_sq_register);
	RUN_TEST(test_delete_io_cq_clears_state_and_register);
	RUN_TEST(test_async_event_request_releases_slot_without_completion);
	RUN_TEST(test_get_log_page_reports_invalid_log_page);
	RUN_TEST(test_completion_register_writes_carry_slot_tag_and_cpl_type);
	RUN_TEST(test_flush_completes_immediately_without_touching_ftl);
	RUN_TEST(test_write_command_creates_slice_requests_and_rx_dmas);
	RUN_TEST(test_read_command_streams_tx_dmas_with_4k_offsets);
	RUN_TEST(test_partial_write_of_unwritten_slice_skips_nand_read);
	RUN_TEST(test_write_spanning_slice_boundary_touches_both_slices);
	return UNITY_END();
}
