/* Unit tests for nvme/nvme_admin_cmd.c and nvme/nvme_identify.c. */
#include "unity.h"

#include <string.h>

#include "fw_test.h"
#include "ftl_config.h"
#include "nvme/nvme.h"
#include "nvme/nvme_admin_cmd.h"
#include "nvme/nvme_identify.h"

extern volatile NVME_CONTEXT g_nvmeTask;

void setUp(void) { fw_test_reset(); }
void tearDown(void) {}

static NVME_COMMAND make_admin_cmd(unsigned char opc, unsigned short cmdSlotTag)
{
	NVME_COMMAND cmd;
	NVME_ADMIN_COMMAND *admin = (NVME_ADMIN_COMMAND *)cmd.cmdDword;

	memset(&cmd, 0, sizeof(cmd));
	cmd.cmdSlotTag = cmdSlotTag;
	admin->OPC = opc;
	return cmd;
}

static NVME_ADMIN_COMMAND *admin_of(NVME_COMMAND *cmd) { return (NVME_ADMIN_COMMAND *)cmd->cmdDword; }

static const mock_host_call_t *expect_single_auto_cpl(unsigned short cmdSlotTag)
{
	const mock_host_call_t *cpl = mock_host_last(MOCK_HOST_SET_AUTO_NVME_CPL);

	TEST_ASSERT_EQUAL_UINT(1, mock_host_count(MOCK_HOST_SET_AUTO_NVME_CPL));
	TEST_ASSERT_NOT_NULL(cpl);
	TEST_ASSERT_EQUAL_UINT(cmdSlotTag, cpl->args[0]);
	return cpl;
}

/* ------------------------------------------------------------------------ */
/* Identify                                                                 */
/* ------------------------------------------------------------------------ */

static void test_smoke_identify_controller_fills_buffer_and_completes(void)
{
	NVME_COMMAND cmd = make_admin_cmd(ADMIN_IDENTIFY, 3);
	NVME_ADMIN_COMMAND *admin = admin_of(&cmd);
	ADMIN_IDENTIFY_CONTROLLER *identify = fw_ptr(ADMIN_CMD_DRAM_DATA_BUFFER);

	admin->dword10 = 1; /* CNS = controller */
	admin->PRP1[0] = 0x1000;

	handle_nvme_admin_cmd(&cmd);

	TEST_ASSERT_EQUAL_HEX16(PCI_VENDOR_ID, identify->VID);
	TEST_ASSERT_EQUAL_UINT(1, mock_host_count(MOCK_HOST_SET_DIRECT_TX_DMA));
	TEST_ASSERT_EQUAL_UINT(1, mock_host_count(MOCK_HOST_CHECK_DIRECT_TX_DMA_DONE));
	expect_single_auto_cpl(3);
}

static void test_identify_controller_dma_covers_full_page_from_aligned_prp1(void)
{
	NVME_COMMAND cmd = make_admin_cmd(ADMIN_IDENTIFY, 1);
	NVME_ADMIN_COMMAND *admin = admin_of(&cmd);
	const mock_host_call_t *dma;

	admin->dword10 = 1;
	admin->PRP1[0] = 0x12345000;
	admin->PRP1[1] = 0x1;

	handle_nvme_admin_cmd(&cmd);

	dma = mock_host_last(MOCK_HOST_SET_DIRECT_TX_DMA);
	TEST_ASSERT_EQUAL_HEX32(ADMIN_CMD_DRAM_DATA_BUFFER, dma->args[0]);
	TEST_ASSERT_EQUAL_HEX32(0x1, dma->args[1]);
	TEST_ASSERT_EQUAL_HEX32(0x12345000, dma->args[2]);
	TEST_ASSERT_EQUAL_UINT(0x1000, dma->args[3]);
}

static void test_identify_namespace_fills_buffer(void)
{
	NVME_COMMAND cmd = make_admin_cmd(ADMIN_IDENTIFY, 4);
	NVME_ADMIN_COMMAND *admin = admin_of(&cmd);
	ADMIN_IDENTIFY_NAMESPACE *ns = fw_ptr(ADMIN_CMD_DRAM_DATA_BUFFER);

	admin->dword10 = 0; /* CNS = namespace */
	admin->NSID = 1;
	admin->PRP1[0] = 0x2000;
	storageCapacity_L = 0x12345;

	handle_nvme_admin_cmd(&cmd);

	TEST_ASSERT_EQUAL_HEX32(0x12345, ns->NSZE[0]);
	TEST_ASSERT_EQUAL_HEX32(STORAGE_CAPACITY_H, ns->NSZE[1]);
	TEST_ASSERT_EQUAL_HEX32(0x12345, ns->NCAP[0]);
	TEST_ASSERT_EQUAL_HEX32(0x12345, ns->NUSE[0]);
	TEST_ASSERT_EQUAL_UINT(1, mock_host_count(MOCK_HOST_SET_DIRECT_TX_DMA));
	expect_single_auto_cpl(4);
}

static void test_identify_with_page_offset_prp1_splits_dma_across_prp2(void)
{
	NVME_COMMAND cmd = make_admin_cmd(ADMIN_IDENTIFY, 2);
	NVME_ADMIN_COMMAND *admin = admin_of(&cmd);
	const mock_host_call_t *first;
	const mock_host_call_t *second;

	admin->dword10 = 1;
	admin->PRP1[0] = 0x1200; /* 16-byte aligned but 0x200 into the page */
	admin->PRP2[0] = 0x5000;
	admin->PRP2[1] = 0;

	handle_nvme_admin_cmd(&cmd);

	TEST_ASSERT_EQUAL_UINT(2, mock_host_count(MOCK_HOST_SET_DIRECT_TX_DMA));
	first = mock_host_call_at(0);
	second = mock_host_call_at(1);
	TEST_ASSERT_EQUAL_UINT(MOCK_HOST_SET_DIRECT_TX_DMA, first->kind);
	TEST_ASSERT_EQUAL_UINT(0x1000 - 0x200, first->args[3]);
	TEST_ASSERT_EQUAL_HEX32(0x1200, first->args[2]);
	TEST_ASSERT_EQUAL_UINT(MOCK_HOST_SET_DIRECT_TX_DMA, second->kind);
	TEST_ASSERT_EQUAL_HEX32(ADMIN_CMD_DRAM_DATA_BUFFER + 0xE00, second->args[0]);
	TEST_ASSERT_EQUAL_HEX32(0x5000, second->args[2]);
	TEST_ASSERT_EQUAL_UINT(0x200, second->args[3]);
}

static void test_identify_rejects_unaligned_prp1(void)
{
	NVME_COMMAND cmd = make_admin_cmd(ADMIN_IDENTIFY, 2);
	NVME_ADMIN_COMMAND *admin = admin_of(&cmd);

	admin->dword10 = 1;
	admin->PRP1[0] = 0x1004;

	FW_EXPECT_ASSERT(handle_nvme_admin_cmd(&cmd));
	TEST_ASSERT_EQUAL_UINT(0, mock_host_count(MOCK_HOST_SET_DIRECT_TX_DMA));
}

static void test_identify_namespace_rejects_unaligned_prp2(void)
{
	NVME_COMMAND cmd = make_admin_cmd(ADMIN_IDENTIFY, 2);
	NVME_ADMIN_COMMAND *admin = admin_of(&cmd);

	admin->dword10 = 0;
	admin->PRP1[0] = 0x1000;
	admin->PRP2[0] = 0x8;

	FW_EXPECT_ASSERT(handle_nvme_admin_cmd(&cmd));
}

static void test_identify_split_rejects_prp2_high_dword_with_page_offset(void)
{
	NVME_COMMAND cmd = make_admin_cmd(ADMIN_IDENTIFY, 2);
	NVME_ADMIN_COMMAND *admin = admin_of(&cmd);

	admin->dword10 = 1;
	admin->PRP1[0] = 0x1100;
	admin->PRP2[0] = 0x5000;
	admin->PRP2[1] = 0x123; /* firmware asserts (prp[1] & 0xFFF) == 0 */

	FW_EXPECT_ASSERT(handle_nvme_admin_cmd(&cmd));
	TEST_ASSERT_EQUAL_UINT(1, mock_host_count(MOCK_HOST_SET_DIRECT_TX_DMA));
}

/* ------------------------------------------------------------------------ */
/* Set Features                                                             */
/* ------------------------------------------------------------------------ */

static void test_get_num_of_queue_passes_small_counts_through(void)
{
	/* NCQR in low half, NSQR in high half (zero based). */
	TEST_ASSERT_EQUAL_HEX32(0x00030002, get_num_of_queue(0x00030002));
	TEST_ASSERT_EQUAL_HEX32(0, get_num_of_queue(0));
}

static void test_get_num_of_queue_clamps_to_max_supported_queues(void)
{
	unsigned int clamped = get_num_of_queue(0xFFFFFFFF);

	TEST_ASSERT_EQUAL_HEX32(((MAX_NUM_OF_IO_SQ - 1) << 16) | (MAX_NUM_OF_IO_CQ - 1), clamped);
	TEST_ASSERT_EQUAL_HEX32(((MAX_NUM_OF_IO_SQ - 1) << 16) | 0x1,
			get_num_of_queue((MAX_NUM_OF_IO_SQ << 16) | 0x1));
	TEST_ASSERT_EQUAL_HEX32((0x2 << 16) | (MAX_NUM_OF_IO_CQ - 1),
			get_num_of_queue((0x2 << 16) | MAX_NUM_OF_IO_CQ));
}

static void test_set_features_number_of_queues_returns_clamped_count(void)
{
	NVME_COMMAND cmd = make_admin_cmd(ADMIN_SET_FEATURES, 7);
	NVME_ADMIN_COMMAND *admin = admin_of(&cmd);
	const mock_host_call_t *cpl;

	admin->dword10 = NUMBER_OF_QUEUES;
	admin->dword11 = 0x00FF00FF;

	handle_nvme_admin_cmd(&cmd);

	cpl = expect_single_auto_cpl(7);
	TEST_ASSERT_EQUAL_HEX32(((MAX_NUM_OF_IO_SQ - 1) << 16) | (MAX_NUM_OF_IO_CQ - 1), cpl->args[1]);
	TEST_ASSERT_EQUAL_UINT(0, cpl->args[2]);
}

static void test_set_features_volatile_write_cache_updates_context(void)
{
	NVME_COMMAND cmd = make_admin_cmd(ADMIN_SET_FEATURES, 1);
	NVME_ADMIN_COMMAND *admin = admin_of(&cmd);

	admin->dword10 = VOLATILE_WRITE_CACHE;
	admin->dword11 = 0x1;
	handle_nvme_admin_cmd(&cmd);
	TEST_ASSERT_EQUAL_UINT(1, g_nvmeTask.cacheEn);

	admin->dword11 = 0xFFFFFFFE; /* only bit 0 matters */
	handle_nvme_admin_cmd(&cmd);
	TEST_ASSERT_EQUAL_UINT(0, g_nvmeTask.cacheEn);
	TEST_ASSERT_EQUAL_UINT(2, mock_host_count(MOCK_HOST_SET_AUTO_NVME_CPL));
}

static void test_set_features_accepted_fids_complete_with_zero_result(void)
{
	static const unsigned char fids[] = {INTERRUPT_COALESCING, ARBITRATION,
			ASYNCHRONOUS_EVENT_CONFIGURATION, POWER_MANAGEMENT};
	unsigned int i;

	for (i = 0; i < sizeof(fids) / sizeof(fids[0]); i++) {
		NVME_COMMAND cmd = make_admin_cmd(ADMIN_SET_FEATURES, (unsigned short)(10 + i));
		const mock_host_call_t *cpl;

		admin_of(&cmd)->dword10 = fids[i];
		admin_of(&cmd)->dword11 = 0xDEADBEEF;
		handle_nvme_admin_cmd(&cmd);

		cpl = mock_host_last(MOCK_HOST_SET_AUTO_NVME_CPL);
		TEST_ASSERT_EQUAL_UINT(10 + i, cpl->args[0]);
		TEST_ASSERT_EQUAL_UINT(0, cpl->args[1]);
		TEST_ASSERT_EQUAL_UINT(0, cpl->args[2]);
	}
	TEST_ASSERT_EQUAL_UINT(4, mock_host_count(MOCK_HOST_SET_AUTO_NVME_CPL));
}

static void test_set_features_unsupported_fid_asserts(void)
{
	NVME_COMMAND cmd = make_admin_cmd(ADMIN_SET_FEATURES, 1);

	admin_of(&cmd)->dword10 = LBA_RANGE_TYPE; /* not settable in this firmware */

	FW_EXPECT_ASSERT(handle_nvme_admin_cmd(&cmd));
	TEST_ASSERT_EQUAL_UINT(0, mock_host_count(MOCK_HOST_SET_AUTO_NVME_CPL));
}

/* ------------------------------------------------------------------------ */
/* Get Features                                                             */
/* ------------------------------------------------------------------------ */

static void test_get_features_lba_range_type_reports_invalid_field(void)
{
	NVME_COMMAND cmd = make_admin_cmd(ADMIN_GET_FEATURES, 9);
	NVME_ADMIN_COMMAND *admin = admin_of(&cmd);
	NVME_COMPLETION expected;
	const mock_host_call_t *cpl;

	admin->dword10 = LBA_RANGE_TYPE;
	admin->NSID = 1;

	handle_nvme_admin_cmd(&cmd);

	expected.dword[0] = 0;
	expected.statusField.SC = SC_INVALID_FIELD_IN_COMMAND;
	cpl = expect_single_auto_cpl(9);
	TEST_ASSERT_EQUAL_UINT(0, cpl->args[1]);
	TEST_ASSERT_EQUAL_HEX16(expected.statusFieldWord, cpl->args[2]);
}

static void test_get_features_lba_range_type_requires_namespace_one(void)
{
	NVME_COMMAND cmd = make_admin_cmd(ADMIN_GET_FEATURES, 9);
	NVME_ADMIN_COMMAND *admin = admin_of(&cmd);

	admin->dword10 = LBA_RANGE_TYPE;
	admin->NSID = 2;

	FW_EXPECT_ASSERT(handle_nvme_admin_cmd(&cmd));
}

static void test_get_features_temperature_threshold_echoes_dword11(void)
{
	NVME_COMMAND cmd = make_admin_cmd(ADMIN_GET_FEATURES, 5);
	NVME_ADMIN_COMMAND *admin = admin_of(&cmd);
	const mock_host_call_t *cpl;

	admin->dword10 = TEMPERATURE_THRESHOLD;
	admin->dword11 = 0x0157;

	handle_nvme_admin_cmd(&cmd);

	cpl = expect_single_auto_cpl(5);
	TEST_ASSERT_EQUAL_HEX32(0x0157, cpl->args[1]);
	TEST_ASSERT_EQUAL_UINT(0, cpl->args[2]);
}

static void test_get_features_volatile_write_cache_reports_context(void)
{
	NVME_COMMAND cmd = make_admin_cmd(ADMIN_GET_FEATURES, 6);
	const mock_host_call_t *cpl;

	admin_of(&cmd)->dword10 = VOLATILE_WRITE_CACHE;
	g_nvmeTask.cacheEn = 1;

	handle_nvme_admin_cmd(&cmd);

	cpl = expect_single_auto_cpl(6);
	TEST_ASSERT_EQUAL_UINT(1, cpl->args[1]);
}

static void test_get_features_power_management_returns_zero(void)
{
	NVME_COMMAND cmd = make_admin_cmd(ADMIN_GET_FEATURES, 6);
	const mock_host_call_t *cpl;

	admin_of(&cmd)->dword10 = POWER_MANAGEMENT;

	handle_nvme_admin_cmd(&cmd);

	cpl = expect_single_auto_cpl(6);
	TEST_ASSERT_EQUAL_UINT(0, cpl->args[1]);
	TEST_ASSERT_EQUAL_UINT(0, cpl->args[2]);
}

static void test_get_features_unsupported_fid_asserts(void)
{
	NVME_COMMAND cmd = make_admin_cmd(ADMIN_GET_FEATURES, 1);

	admin_of(&cmd)->dword10 = NUMBER_OF_QUEUES; /* only settable in this firmware */

	FW_EXPECT_ASSERT(handle_nvme_admin_cmd(&cmd));
}

/* ------------------------------------------------------------------------ */
/* I/O queue create / delete                                                */
/* ------------------------------------------------------------------------ */

static void test_create_io_sq_records_status_and_programs_controller(void)
{
	NVME_COMMAND cmd = make_admin_cmd(ADMIN_CREATE_IO_SQ, 11);
	NVME_ADMIN_COMMAND *admin = admin_of(&cmd);
	const mock_host_call_t *sq;

	admin->PRP1[0] = 0xABCDE000;
	admin->PRP1[1] = 0x3;
	admin->dword10 = (0x3F << 16) | 0x2; /* QSIZE = 63, QID = 2 */
	admin->dword11 = (0x5 << 16) | 0x1;  /* CQID = 5, PC = 1 */

	handle_nvme_admin_cmd(&cmd);

	TEST_ASSERT_EQUAL_UINT(1, g_nvmeTask.ioSqInfo[1].valid);
	TEST_ASSERT_EQUAL_UINT(0x3F, g_nvmeTask.ioSqInfo[1].qSzie);
	TEST_ASSERT_EQUAL_UINT(0x5, g_nvmeTask.ioSqInfo[1].cqVector);
	TEST_ASSERT_EQUAL_HEX32(0xABCDE000, g_nvmeTask.ioSqInfo[1].pcieBaseAddrL);
	TEST_ASSERT_EQUAL_HEX32(0x3, g_nvmeTask.ioSqInfo[1].pcieBaseAddrH);

	sq = mock_host_last(MOCK_HOST_SET_IO_SQ);
	TEST_ASSERT_EQUAL_UINT(1, mock_host_count(MOCK_HOST_SET_IO_SQ));
	TEST_ASSERT_EQUAL_UINT(1, sq->args[0]);
	TEST_ASSERT_EQUAL_UINT(1, sq->args[1]);
	TEST_ASSERT_EQUAL_UINT(5, sq->args[2]);
	TEST_ASSERT_EQUAL_UINT(0x3F, sq->args[3]);
	TEST_ASSERT_EQUAL_HEX32(0xABCDE000, sq->args[4]);
	TEST_ASSERT_EQUAL_HEX32(0x3, sq->args[5]);
	expect_single_auto_cpl(11);
}

static void test_create_io_sq_accepts_boundary_queue_ids(void)
{
	NVME_COMMAND cmd = make_admin_cmd(ADMIN_CREATE_IO_SQ, 1);
	NVME_ADMIN_COMMAND *admin = admin_of(&cmd);

	admin->PRP1[0] = 0x1000;
	admin->dword10 = (0xFF << 16) | 0x8; /* max QSIZE, max QID */
	admin->dword11 = (0x8 << 16) | 0x1;  /* max CQID */
	handle_nvme_admin_cmd(&cmd);
	TEST_ASSERT_EQUAL_UINT(1, g_nvmeTask.ioSqInfo[7].valid);

	admin->dword10 = 0x1;
	admin->dword11 = (0x1 << 16);
	handle_nvme_admin_cmd(&cmd);
	TEST_ASSERT_EQUAL_UINT(1, g_nvmeTask.ioSqInfo[0].valid);
	TEST_ASSERT_EQUAL_UINT(0, g_nvmeTask.ioSqInfo[0].qSzie);
}

static void test_create_io_sq_rejects_invalid_parameters(void)
{
	NVME_COMMAND cmd = make_admin_cmd(ADMIN_CREATE_IO_SQ, 1);
	NVME_ADMIN_COMMAND *admin = admin_of(&cmd);

	admin->PRP1[0] = 0x1000;
	admin->dword10 = (0x10 << 16) | 0x0; /* QID 0 is the admin queue */
	admin->dword11 = (0x1 << 16);
	FW_EXPECT_ASSERT(handle_nvme_admin_cmd(&cmd));

	admin->dword10 = (0x10 << 16) | 0x9; /* QID > 8 */
	FW_EXPECT_ASSERT(handle_nvme_admin_cmd(&cmd));

	admin->dword10 = (0x100 << 16) | 0x1; /* QSIZE too large */
	FW_EXPECT_ASSERT(handle_nvme_admin_cmd(&cmd));

	admin->dword10 = (0x10 << 16) | 0x1;
	admin->dword11 = 0; /* CQID 0 */
	FW_EXPECT_ASSERT(handle_nvme_admin_cmd(&cmd));

	admin->dword11 = (0x9 << 16); /* CQID > 8 */
	FW_EXPECT_ASSERT(handle_nvme_admin_cmd(&cmd));

	admin->dword11 = (0x1 << 16);
	admin->PRP1[0] = 0x1008; /* unaligned base */
	FW_EXPECT_ASSERT(handle_nvme_admin_cmd(&cmd));

	admin->PRP1[0] = 0x1000;
	admin->PRP1[1] = 0x10; /* base above the supported 36-bit window */
	FW_EXPECT_ASSERT(handle_nvme_admin_cmd(&cmd));

	TEST_ASSERT_EQUAL_UINT(0, mock_host_count(MOCK_HOST_SET_IO_SQ));
	TEST_ASSERT_EQUAL_UINT(0, mock_host_count(MOCK_HOST_SET_AUTO_NVME_CPL));
}

static void test_delete_io_sq_clears_status_and_controller(void)
{
	NVME_COMMAND create = make_admin_cmd(ADMIN_CREATE_IO_SQ, 1);
	NVME_COMMAND del = make_admin_cmd(ADMIN_DELETE_IO_SQ, 2);
	const mock_host_call_t *sq;

	admin_of(&create)->PRP1[0] = 0x1000;
	admin_of(&create)->dword10 = (0x10 << 16) | 0x3;
	admin_of(&create)->dword11 = (0x3 << 16);
	handle_nvme_admin_cmd(&create);
	TEST_ASSERT_EQUAL_UINT(1, g_nvmeTask.ioSqInfo[2].valid);

	admin_of(&del)->dword10 = 0x3;
	handle_nvme_admin_cmd(&del);

	TEST_ASSERT_EQUAL_UINT(0, g_nvmeTask.ioSqInfo[2].valid);
	TEST_ASSERT_EQUAL_UINT(0, g_nvmeTask.ioSqInfo[2].qSzie);
	TEST_ASSERT_EQUAL_UINT(0, g_nvmeTask.ioSqInfo[2].cqVector);
	TEST_ASSERT_EQUAL_UINT(0, g_nvmeTask.ioSqInfo[2].pcieBaseAddrL);
	TEST_ASSERT_EQUAL_UINT(0, g_nvmeTask.ioSqInfo[2].pcieBaseAddrH);

	sq = mock_host_last(MOCK_HOST_SET_IO_SQ);
	TEST_ASSERT_EQUAL_UINT(2, mock_host_count(MOCK_HOST_SET_IO_SQ));
	TEST_ASSERT_EQUAL_UINT(2, sq->args[0]);
	TEST_ASSERT_EQUAL_UINT(0, sq->args[1]);
	TEST_ASSERT_EQUAL_UINT(0, sq->args[4]);
	TEST_ASSERT_EQUAL_UINT(2, mock_host_last(MOCK_HOST_SET_AUTO_NVME_CPL)->args[0]);
}

static void test_create_io_cq_records_status_and_programs_controller(void)
{
	NVME_COMMAND cmd = make_admin_cmd(ADMIN_CREATE_IO_CQ, 12);
	NVME_ADMIN_COMMAND *admin = admin_of(&cmd);
	const mock_host_call_t *cq;

	admin->PRP1[0] = 0x7FFF0000;
	admin->PRP1[1] = 0xF;
	admin->dword10 = (0x7F << 16) | 0x4;      /* QSIZE = 127, QID = 4 */
	admin->dword11 = (0x6 << 16) | 0x2 | 0x1; /* IV = 6, IEN = 1, PC = 1 */

	handle_nvme_admin_cmd(&cmd);

	TEST_ASSERT_EQUAL_UINT(1, g_nvmeTask.ioCqInfo[3].valid);
	TEST_ASSERT_EQUAL_UINT(0x7F, g_nvmeTask.ioCqInfo[3].qSzie);
	TEST_ASSERT_EQUAL_UINT(1, g_nvmeTask.ioCqInfo[3].irqEn);
	TEST_ASSERT_EQUAL_UINT(6, g_nvmeTask.ioCqInfo[3].irqVector);
	TEST_ASSERT_EQUAL_HEX32(0x7FFF0000, g_nvmeTask.ioCqInfo[3].pcieBaseAddrL);
	TEST_ASSERT_EQUAL_HEX32(0xF, g_nvmeTask.ioCqInfo[3].pcieBaseAddrH);

	cq = mock_host_last(MOCK_HOST_SET_IO_CQ);
	TEST_ASSERT_EQUAL_UINT(3, cq->args[0]);
	TEST_ASSERT_EQUAL_UINT(1, cq->args[1]);
	TEST_ASSERT_EQUAL_UINT(1, cq->args[2]);
	TEST_ASSERT_EQUAL_UINT(6, cq->args[3]);
	TEST_ASSERT_EQUAL_UINT(0x7F, cq->args[4]);
	TEST_ASSERT_EQUAL_HEX32(0x7FFF0000, cq->args[5]);
	TEST_ASSERT_EQUAL_HEX32(0xF, cq->args[6]);
	expect_single_auto_cpl(12);
}

static void test_create_io_cq_without_interrupts_keeps_irq_disabled(void)
{
	NVME_COMMAND cmd = make_admin_cmd(ADMIN_CREATE_IO_CQ, 1);
	NVME_ADMIN_COMMAND *admin = admin_of(&cmd);

	admin->PRP1[0] = 0x1000;
	admin->dword10 = (0x1 << 16) | 0x1;
	admin->dword11 = 0x1; /* PC only */

	handle_nvme_admin_cmd(&cmd);

	TEST_ASSERT_EQUAL_UINT(0, g_nvmeTask.ioCqInfo[0].irqEn);
	TEST_ASSERT_EQUAL_UINT(0, g_nvmeTask.ioCqInfo[0].irqVector);
	TEST_ASSERT_EQUAL_UINT(0, mock_host_last(MOCK_HOST_SET_IO_CQ)->args[2]);
}

static void test_create_io_cq_rejects_invalid_parameters(void)
{
	NVME_COMMAND cmd = make_admin_cmd(ADMIN_CREATE_IO_CQ, 1);
	NVME_ADMIN_COMMAND *admin = admin_of(&cmd);

	admin->PRP1[0] = 0x1000;
	admin->dword10 = (0x10 << 16) | 0x1;
	admin->dword11 = (0x8 << 16); /* IV >= 8 */
	FW_EXPECT_ASSERT(handle_nvme_admin_cmd(&cmd));

	admin->dword11 = 0;
	admin->dword10 = (0x100 << 16) | 0x1; /* QSIZE too large */
	FW_EXPECT_ASSERT(handle_nvme_admin_cmd(&cmd));

	admin->dword10 = (0x10 << 16) | 0x0; /* QID 0 */
	FW_EXPECT_ASSERT(handle_nvme_admin_cmd(&cmd));

	admin->dword10 = (0x10 << 16) | 0x9; /* QID > 8 */
	FW_EXPECT_ASSERT(handle_nvme_admin_cmd(&cmd));

	admin->dword10 = (0x10 << 16) | 0x1;
	admin->PRP1[0] = 0x1001;
	FW_EXPECT_ASSERT(handle_nvme_admin_cmd(&cmd));

	TEST_ASSERT_EQUAL_UINT(0, mock_host_count(MOCK_HOST_SET_IO_CQ));
}

static void test_delete_io_cq_clears_status_and_controller(void)
{
	NVME_COMMAND create = make_admin_cmd(ADMIN_CREATE_IO_CQ, 1);
	NVME_COMMAND del = make_admin_cmd(ADMIN_DELETE_IO_CQ, 3);
	const mock_host_call_t *cq;

	admin_of(&create)->PRP1[0] = 0x1000;
	admin_of(&create)->dword10 = (0x10 << 16) | 0x8;
	admin_of(&create)->dword11 = (0x7 << 16) | 0x2;
	handle_nvme_admin_cmd(&create);
	TEST_ASSERT_EQUAL_UINT(1, g_nvmeTask.ioCqInfo[7].valid);

	admin_of(&del)->dword10 = 0x8;
	handle_nvme_admin_cmd(&del);

	TEST_ASSERT_EQUAL_UINT(0, g_nvmeTask.ioCqInfo[7].valid);
	TEST_ASSERT_EQUAL_UINT(0, g_nvmeTask.ioCqInfo[7].irqVector);
	TEST_ASSERT_EQUAL_UINT(0, g_nvmeTask.ioCqInfo[7].qSzie);
	TEST_ASSERT_EQUAL_UINT(0, g_nvmeTask.ioCqInfo[7].pcieBaseAddrL);

	cq = mock_host_last(MOCK_HOST_SET_IO_CQ);
	TEST_ASSERT_EQUAL_UINT(2, mock_host_count(MOCK_HOST_SET_IO_CQ));
	TEST_ASSERT_EQUAL_UINT(7, cq->args[0]);
	TEST_ASSERT_EQUAL_UINT(0, cq->args[1]);
	TEST_ASSERT_EQUAL_UINT(3, mock_host_last(MOCK_HOST_SET_AUTO_NVME_CPL)->args[0]);
}

/* ------------------------------------------------------------------------ */
/* Other opcodes                                                            */
/* ------------------------------------------------------------------------ */

static void test_async_event_request_releases_slot_without_completion(void)
{
	NVME_COMMAND cmd = make_admin_cmd(ADMIN_ASYNCHRONOUS_EVENT_REQUEST, 21);

	handle_nvme_admin_cmd(&cmd);

	TEST_ASSERT_EQUAL_UINT(0, mock_host_count(MOCK_HOST_SET_AUTO_NVME_CPL));
	TEST_ASSERT_EQUAL_UINT(0, mock_host_count(MOCK_HOST_SET_NVME_CPL));
	TEST_ASSERT_EQUAL_UINT(1, mock_host_count(MOCK_HOST_SET_NVME_SLOT_RELEASE));
	TEST_ASSERT_EQUAL_UINT(21, mock_host_last(MOCK_HOST_SET_NVME_SLOT_RELEASE)->args[0]);
}

static void test_get_log_page_reports_invalid_log_page(void)
{
	NVME_COMMAND cmd = make_admin_cmd(ADMIN_GET_LOG_PAGE, 8);
	const mock_host_call_t *cpl;

	admin_of(&cmd)->dword10 = 0x2; /* SMART / Health */

	handle_nvme_admin_cmd(&cmd);

	cpl = expect_single_auto_cpl(8);
	TEST_ASSERT_EQUAL_HEX32(0x9, cpl->args[1]);
	TEST_ASSERT_EQUAL_UINT(0, cpl->args[2]);
}

static void test_unsupported_admin_opcodes_assert_without_completion(void)
{
	static const unsigned char opcodes[] = {ADMIN_ABORT, ADMIN_FIRMWARE_ACTIVATE,
			ADMIN_FIRMWARE_IMAGE_DOWNLOAD, ADMIN_FORMAT_NVM, ADMIN_SECURITY_SEND,
			ADMIN_SECURITY_RECEIVE, 0xFF};
	unsigned int i;

	for (i = 0; i < sizeof(opcodes) / sizeof(opcodes[0]); i++) {
		NVME_COMMAND cmd = make_admin_cmd(opcodes[i], 1);

		FW_EXPECT_ASSERT(handle_nvme_admin_cmd(&cmd));
	}
	TEST_ASSERT_EQUAL_UINT(sizeof(opcodes) / sizeof(opcodes[0]), fw_assert_count());
	TEST_ASSERT_EQUAL_UINT(0, mock_host_call_count());
}

int main(void)
{
	UNITY_BEGIN();
	RUN_TEST(test_smoke_identify_controller_fills_buffer_and_completes);
	RUN_TEST(test_identify_controller_dma_covers_full_page_from_aligned_prp1);
	RUN_TEST(test_identify_namespace_fills_buffer);
	RUN_TEST(test_identify_with_page_offset_prp1_splits_dma_across_prp2);
	RUN_TEST(test_identify_rejects_unaligned_prp1);
	RUN_TEST(test_identify_namespace_rejects_unaligned_prp2);
	RUN_TEST(test_identify_split_rejects_prp2_high_dword_with_page_offset);
	RUN_TEST(test_get_num_of_queue_passes_small_counts_through);
	RUN_TEST(test_get_num_of_queue_clamps_to_max_supported_queues);
	RUN_TEST(test_set_features_number_of_queues_returns_clamped_count);
	RUN_TEST(test_set_features_volatile_write_cache_updates_context);
	RUN_TEST(test_set_features_accepted_fids_complete_with_zero_result);
	RUN_TEST(test_set_features_unsupported_fid_asserts);
	RUN_TEST(test_get_features_lba_range_type_reports_invalid_field);
	RUN_TEST(test_get_features_lba_range_type_requires_namespace_one);
	RUN_TEST(test_get_features_temperature_threshold_echoes_dword11);
	RUN_TEST(test_get_features_volatile_write_cache_reports_context);
	RUN_TEST(test_get_features_power_management_returns_zero);
	RUN_TEST(test_get_features_unsupported_fid_asserts);
	RUN_TEST(test_create_io_sq_records_status_and_programs_controller);
	RUN_TEST(test_create_io_sq_accepts_boundary_queue_ids);
	RUN_TEST(test_create_io_sq_rejects_invalid_parameters);
	RUN_TEST(test_delete_io_sq_clears_status_and_controller);
	RUN_TEST(test_create_io_cq_records_status_and_programs_controller);
	RUN_TEST(test_create_io_cq_without_interrupts_keeps_irq_disabled);
	RUN_TEST(test_create_io_cq_rejects_invalid_parameters);
	RUN_TEST(test_delete_io_cq_clears_status_and_controller);
	RUN_TEST(test_async_event_request_releases_slot_without_completion);
	RUN_TEST(test_get_log_page_reports_invalid_log_page);
	RUN_TEST(test_unsupported_admin_opcodes_assert_without_completion);
	return UNITY_END();
}
