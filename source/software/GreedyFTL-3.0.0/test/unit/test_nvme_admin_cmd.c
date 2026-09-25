/* Unit tests for nvme/nvme_admin_cmd.c and nvme/nvme_identify.c. */
#include "unity.h"

#include <string.h>

#include "fw_test.h"
#include "ftl_config.h"
#include "nvme/nvme.h"
#include "nvme/nvme_admin_cmd.h"
#include "nvme/nvme_identify.h"

extern NVME_CONTEXT g_nvmeTask;

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

/* Asserts the command completed through the auto-completion path exactly once. */
static void expect_auto_cpl(unsigned short slotTag, unsigned int specific, unsigned int statusFieldWord)
{
	const mock_host_call_t *cpl = mock_host_last(MOCK_HOST_SET_AUTO_NVME_CPL);

	TEST_ASSERT_EQUAL_UINT(1, mock_host_count(MOCK_HOST_SET_AUTO_NVME_CPL));
	TEST_ASSERT_EQUAL_UINT(0, mock_host_count(MOCK_HOST_SET_NVME_SLOT_RELEASE));
	TEST_ASSERT_EQUAL_UINT(0, mock_host_count(MOCK_HOST_SET_NVME_CPL));
	TEST_ASSERT_NOT_NULL(cpl);
	TEST_ASSERT_EQUAL_UINT(slotTag, cpl->args[0]);
	TEST_ASSERT_EQUAL_HEX32(specific, cpl->args[1]);
	TEST_ASSERT_EQUAL_HEX32(statusFieldWord, cpl->args[2]);
}

/* ------------------------------------------------------------------ */
/* Identify                                                           */
/* ------------------------------------------------------------------ */

static void test_smoke_identify_controller_fills_buffer_and_completes(void)
{
	NVME_COMMAND cmd = make_admin_cmd(ADMIN_IDENTIFY, 3);
	NVME_ADMIN_COMMAND *admin = admin_of(&cmd);
	ADMIN_IDENTIFY_CONTROLLER *identify = fw_ptr(ADMIN_CMD_DRAM_DATA_BUFFER);
	const mock_host_call_t *dma;

	admin->dword10 = 1; /* CNS = controller */
	admin->PRP1[0] = 0x1000;
	admin->PRP1[1] = 0x2;

	handle_nvme_admin_cmd(&cmd);

	TEST_ASSERT_EQUAL_HEX16(PCI_VENDOR_ID, identify->VID);
	TEST_ASSERT_EQUAL_UINT(1, mock_host_count(MOCK_HOST_SET_DIRECT_TX_DMA));
	dma = mock_host_last(MOCK_HOST_SET_DIRECT_TX_DMA);
	TEST_ASSERT_EQUAL_HEX32(ADMIN_CMD_DRAM_DATA_BUFFER, dma->args[0]);
	TEST_ASSERT_EQUAL_HEX32(0x2, dma->args[1]);
	TEST_ASSERT_EQUAL_HEX32(0x1000, dma->args[2]);
	TEST_ASSERT_EQUAL_HEX32(0x1000, dma->args[3]);
	TEST_ASSERT_EQUAL_UINT(1, mock_host_count(MOCK_HOST_CHECK_DIRECT_TX_DMA_DONE));
	expect_auto_cpl(3, 0, 0);
}

static void test_identify_namespace_fills_buffer_and_completes(void)
{
	NVME_COMMAND cmd = make_admin_cmd(ADMIN_IDENTIFY, 7);
	NVME_ADMIN_COMMAND *admin = admin_of(&cmd);
	ADMIN_IDENTIFY_NAMESPACE *ns = fw_ptr(ADMIN_CMD_DRAM_DATA_BUFFER);

	storageCapacity_L = 0x12345678;
	admin->dword10 = 0; /* CNS = namespace */
	admin->NSID = 1;
	admin->PRP1[0] = 0x8000;

	handle_nvme_admin_cmd(&cmd);

	TEST_ASSERT_EQUAL_HEX32(0x12345678, ns->NSZE[0]);
	TEST_ASSERT_EQUAL_HEX32(STORAGE_CAPACITY_H, ns->NSZE[1]);
	TEST_ASSERT_EQUAL_UINT(1, mock_host_count(MOCK_HOST_SET_DIRECT_TX_DMA));
	TEST_ASSERT_EQUAL_HEX32(0x8000, mock_host_last(MOCK_HOST_SET_DIRECT_TX_DMA)->args[2]);
	expect_auto_cpl(7, 0, 0);
}

static void test_identify_ignores_upper_cns_bits(void)
{
	/* ADMIN_IDENTIFY_COMMAND_DW10.CNS is a single bit; bits 31:1 are reserved
	 * and must not change the dispatch. */
	NVME_COMMAND cmd = make_admin_cmd(ADMIN_IDENTIFY, 1);
	NVME_ADMIN_COMMAND *admin = admin_of(&cmd);
	ADMIN_IDENTIFY_CONTROLLER *identify = fw_ptr(ADMIN_CMD_DRAM_DATA_BUFFER);

	admin->dword10 = 0xFFFFFFFF; /* CNS bit set */
	admin->PRP1[0] = 0x1000;

	handle_nvme_admin_cmd(&cmd);

	TEST_ASSERT_EQUAL_HEX16(PCI_VENDOR_ID, identify->VID);
	TEST_ASSERT_EQUAL_HEX16(PCI_SUBSYSTEM_VENDOR_ID, identify->SSVID);
	expect_auto_cpl(1, 0, 0);
}

static void test_identify_split_prp_issues_two_dmas(void)
{
	NVME_COMMAND cmd = make_admin_cmd(ADMIN_IDENTIFY, 2);
	NVME_ADMIN_COMMAND *admin = admin_of(&cmd);
	const mock_host_call_t *first;
	const mock_host_call_t *second;

	admin->dword10 = 1;
	admin->PRP1[0] = 0x1000 + 0x100; /* 16-byte aligned, 0x100 into the page */
	admin->PRP1[1] = 0x1;
	admin->PRP2[0] = 0x5000;
	admin->PRP2[1] = 0x3000; /* low 12 bits of PRP2 high dword must be 0 */

	handle_nvme_admin_cmd(&cmd);

	TEST_ASSERT_EQUAL_UINT(2, mock_host_count(MOCK_HOST_SET_DIRECT_TX_DMA));
	first = mock_host_call_at(0);
	second = mock_host_call_at(1);
	TEST_ASSERT_EQUAL(MOCK_HOST_SET_DIRECT_TX_DMA, first->kind);
	TEST_ASSERT_EQUAL_HEX32(ADMIN_CMD_DRAM_DATA_BUFFER, first->args[0]);
	TEST_ASSERT_EQUAL_HEX32(0x1, first->args[1]);
	TEST_ASSERT_EQUAL_HEX32(0x1100, first->args[2]);
	TEST_ASSERT_EQUAL_HEX32(0x1000 - 0x100, first->args[3]);
	TEST_ASSERT_EQUAL(MOCK_HOST_SET_DIRECT_TX_DMA, second->kind);
	TEST_ASSERT_EQUAL_HEX32(ADMIN_CMD_DRAM_DATA_BUFFER + 0xF00, second->args[0]);
	TEST_ASSERT_EQUAL_HEX32(0x3000, second->args[1]);
	TEST_ASSERT_EQUAL_HEX32(0x5000, second->args[2]);
	TEST_ASSERT_EQUAL_HEX32(0x100, second->args[3]);
	TEST_ASSERT_EQUAL(MOCK_HOST_CHECK_DIRECT_TX_DMA_DONE, mock_host_call_at(2)->kind);
	expect_auto_cpl(2, 0, 0);
}

static void test_identify_split_prp_asserts_on_misaligned_prp2_high(void)
{
	NVME_COMMAND cmd = make_admin_cmd(ADMIN_IDENTIFY, 2);
	NVME_ADMIN_COMMAND *admin = admin_of(&cmd);

	admin->dword10 = 1;
	admin->PRP1[0] = 0x1100;
	admin->PRP2[0] = 0x5000;
	admin->PRP2[1] = 0x3001;

	FW_EXPECT_ASSERT(handle_nvme_admin_cmd(&cmd));
	TEST_ASSERT_EQUAL_UINT(1, mock_host_count(MOCK_HOST_SET_DIRECT_TX_DMA));
	TEST_ASSERT_EQUAL_UINT(0, mock_host_count(MOCK_HOST_SET_AUTO_NVME_CPL));
}

static void test_identify_controller_asserts_on_misaligned_prp1(void)
{
	NVME_COMMAND cmd = make_admin_cmd(ADMIN_IDENTIFY, 2);
	NVME_ADMIN_COMMAND *admin = admin_of(&cmd);

	admin->dword10 = 1;
	admin->PRP1[0] = 0x1004;

	FW_EXPECT_ASSERT(handle_nvme_admin_cmd(&cmd));
	TEST_ASSERT_EQUAL_UINT(0, mock_host_count(MOCK_HOST_SET_DIRECT_TX_DMA));
	TEST_ASSERT_EQUAL_UINT(0, mock_host_count(MOCK_HOST_SET_AUTO_NVME_CPL));
}

static void test_identify_namespace_asserts_on_misaligned_prp2(void)
{
	NVME_COMMAND cmd = make_admin_cmd(ADMIN_IDENTIFY, 2);
	NVME_ADMIN_COMMAND *admin = admin_of(&cmd);

	admin->dword10 = 0;
	admin->PRP1[0] = 0x1000;
	admin->PRP2[0] = 0x2008;

	FW_EXPECT_ASSERT(handle_nvme_admin_cmd(&cmd));
	TEST_ASSERT_EQUAL_UINT(0, mock_host_count(MOCK_HOST_SET_DIRECT_TX_DMA));
}

/* ------------------------------------------------------------------ */
/* Identify data structures (nvme_identify.c)                         */
/* ------------------------------------------------------------------ */

static void test_identify_controller_data_fields(void)
{
	ADMIN_IDENTIFY_CONTROLLER *id = fw_ptr(ADMIN_CMD_DRAM_DATA_BUFFER);
	char sn[21];
	char mn[41];
	char fr[9];

	memset(id, 0xAA, sizeof(*id));
	identify_controller(ADMIN_CMD_DRAM_DATA_BUFFER);

	TEST_ASSERT_EQUAL_HEX16(PCI_VENDOR_ID, id->VID);
	TEST_ASSERT_EQUAL_HEX16(PCI_SUBSYSTEM_VENDOR_ID, id->SSVID);

	memcpy(sn, id->SN, 20);
	sn[20] = 0;
	memcpy(mn, id->MN, 40);
	mn[40] = 0;
	memcpy(fr, id->FR, 8);
	fr[8] = 0;
	TEST_ASSERT_EQUAL_STRING_LEN(SERIAL_NUMBER, sn, strlen(SERIAL_NUMBER));
	TEST_ASSERT_EQUAL_STRING_LEN(MODEL_NUMBER, mn, strlen(MODEL_NUMBER));
	TEST_ASSERT_EQUAL_STRING_LEN(FIRMWARE_REVISION, fr, strlen(FIRMWARE_REVISION));
	/* Trailing space padding: the NUL after the literal is copied, remaining bytes are 0x20. */
	TEST_ASSERT_EQUAL_HEX8(0x00, (unsigned char)sn[strlen(SERIAL_NUMBER)]);
	TEST_ASSERT_EQUAL_HEX8(0x20, (unsigned char)sn[19]);
	TEST_ASSERT_EQUAL_HEX8(0x20, (unsigned char)mn[39]);

	TEST_ASSERT_EQUAL_HEX8(0, id->RAB);
	TEST_ASSERT_EQUAL_HEX8(0xE4, id->IEEE[0]);
	TEST_ASSERT_EQUAL_HEX8(0xD2, id->IEEE[1]);
	TEST_ASSERT_EQUAL_HEX8(0x5C, id->IEEE[2]);
	TEST_ASSERT_EQUAL_HEX8(0x8, id->MDTS);
	TEST_ASSERT_EQUAL_HEX16(0x9, id->CNTLID);
	TEST_ASSERT_EQUAL_HEX8(0x3, id->ACL);
	TEST_ASSERT_EQUAL_HEX8(0x3, id->AERL);
	TEST_ASSERT_EQUAL_UINT(1, id->FRMW.firstFirmwareSlotReadOnly);
	TEST_ASSERT_EQUAL_UINT(1, id->FRMW.supportedNumberOfFirmwareSlots);
	TEST_ASSERT_EQUAL_HEX8(0x8, id->ELPE);
	TEST_ASSERT_EQUAL_HEX8(0, id->NPSS);
	TEST_ASSERT_EQUAL_UINT(6, id->SQES.requiredSubmissionQueueEntrySize);
	TEST_ASSERT_EQUAL_UINT(6, id->SQES.maximumSubmissionQueueEntrySize);
	TEST_ASSERT_EQUAL_UINT(4, id->CQES.requiredCompletionQueueEntrySize);
	TEST_ASSERT_EQUAL_UINT(4, id->CQES.maximumCompletionQueueEntrySize);
	TEST_ASSERT_EQUAL_UINT(1, id->NN);
	TEST_ASSERT_EQUAL_UINT(1, id->VWC.present);
	TEST_ASSERT_EQUAL_UINT(0, id->OACS.supportsFormatNVM);
	TEST_ASSERT_EQUAL_UINT(0, id->ONCS.supportsCompare);
	TEST_ASSERT_EQUAL_UINT(0, id->SGLS.supportsSGL);
	TEST_ASSERT_EQUAL_HEX16(0x09C4, id->PSDx[0].MP);
	TEST_ASSERT_EQUAL_UINT(0, id->PSDx[0].MPS);
	TEST_ASSERT_EQUAL_UINT(0, id->PSDx[0].ENLAT);
	TEST_ASSERT_EQUAL_UINT(0, id->PSDx[0].EXLAT);
	/* Whole structure was cleared before formatting. */
	TEST_ASSERT_EQUAL_HEX16(0, id->PSDx[1].MP);
	TEST_ASSERT_EQUAL_HEX16(0, id->AWUN);
}

static void test_identify_namespace_data_fields(void)
{
	ADMIN_IDENTIFY_NAMESPACE *ns = fw_ptr(ADMIN_CMD_DRAM_DATA_BUFFER);

	storageCapacity_L = 0x00ABCDEF;
	memset(ns, 0xAA, sizeof(*ns));
	identify_namespace(ADMIN_CMD_DRAM_DATA_BUFFER);

	TEST_ASSERT_EQUAL_HEX32(0x00ABCDEF, ns->NSZE[0]);
	TEST_ASSERT_EQUAL_HEX32(STORAGE_CAPACITY_H, ns->NSZE[1]);
	TEST_ASSERT_EQUAL_HEX32(0x00ABCDEF, ns->NCAP[0]);
	TEST_ASSERT_EQUAL_HEX32(STORAGE_CAPACITY_H, ns->NCAP[1]);
	TEST_ASSERT_EQUAL_HEX32(0x00ABCDEF, ns->NUSE[0]);
	TEST_ASSERT_EQUAL_HEX32(STORAGE_CAPACITY_H, ns->NUSE[1]);
	TEST_ASSERT_EQUAL_UINT(0, ns->NSFEAT.supportsThinProvisioning);
	TEST_ASSERT_EQUAL_HEX8(0, ns->NLBAF);
	TEST_ASSERT_EQUAL_UINT(0, ns->FLBAS.supportedCombination);
	TEST_ASSERT_EQUAL_UINT(0, ns->FLBAS.supportsMetadataAtEndOfLBA);
	TEST_ASSERT_EQUAL_UINT(0, ns->MC.supportsMetadataAsPartOfLBA);
	TEST_ASSERT_EQUAL_UINT(0, ns->DPC.supportsProtectionType1);
	TEST_ASSERT_EQUAL_UINT(0, ns->DPS.protectionEnabled);
	TEST_ASSERT_EQUAL_UINT(0, ns->NMIC.supportsMultipathIOSharing);
	TEST_ASSERT_EQUAL_UINT(0, ns->RESCAP.supportsPersistThroughPowerLoss);
	TEST_ASSERT_EQUAL_HEX16(0, ns->LBAFx[0].MS);
	TEST_ASSERT_EQUAL_HEX8(0xC, ns->LBAFx[0].LBADS); /* 4 KiB LBA */
	TEST_ASSERT_EQUAL_UINT(2, ns->LBAFx[0].RP);
	TEST_ASSERT_EQUAL_HEX8(0, ns->LBAFx[1].LBADS);
}

static void test_identify_namespace_tracks_ftl_capacity(void)
{
	ADMIN_IDENTIFY_NAMESPACE *ns = fw_ptr(ADMIN_CMD_DRAM_DATA_BUFFER);

	fw_test_init_ftl();
	identify_namespace(ADMIN_CMD_DRAM_DATA_BUFFER);

	TEST_ASSERT_NOT_EQUAL(0, storageCapacity_L);
	TEST_ASSERT_EQUAL_HEX32(storageCapacity_L, ns->NSZE[0]);
	TEST_ASSERT_EQUAL_HEX32(storageCapacity_L, ns->NCAP[0]);
	TEST_ASSERT_EQUAL_HEX32(storageCapacity_L, ns->NUSE[0]);
}

/* ------------------------------------------------------------------ */
/* Set Features                                                       */
/* ------------------------------------------------------------------ */

static void test_get_num_of_queue_passes_small_counts_through(void)
{
	ADMIN_SET_FEATURES_NUMBER_OF_QUEUES_DW11 dw11;

	dw11.NSQR = 3;
	dw11.NCQR = 5;
	TEST_ASSERT_EQUAL_HEX32(dw11.dword, get_num_of_queue(dw11.dword));
	TEST_ASSERT_EQUAL_HEX32(0x00030005, get_num_of_queue(dw11.dword));
}

static void test_get_num_of_queue_clamps_to_max(void)
{
	ADMIN_SET_FEATURES_NUMBER_OF_QUEUES_DW11 dw11;
	ADMIN_SET_FEATURES_NUMBER_OF_QUEUES_DW11 out;

	dw11.NSQR = 0xFFFF;
	dw11.NCQR = MAX_NUM_OF_IO_CQ; /* boundary: equal to max is clamped */
	out.dword = get_num_of_queue(dw11.dword);
	TEST_ASSERT_EQUAL_UINT(MAX_NUM_OF_IO_SQ - 1, out.NSQR);
	TEST_ASSERT_EQUAL_UINT(MAX_NUM_OF_IO_CQ - 1, out.NCQR);

	dw11.NSQR = MAX_NUM_OF_IO_SQ - 1;
	dw11.NCQR = 0;
	out.dword = get_num_of_queue(dw11.dword);
	TEST_ASSERT_EQUAL_UINT(MAX_NUM_OF_IO_SQ - 1, out.NSQR);
	TEST_ASSERT_EQUAL_UINT(0, out.NCQR);
}

static void test_set_features_number_of_queues_reports_clamped_count(void)
{
	NVME_COMMAND cmd = make_admin_cmd(ADMIN_SET_FEATURES, 4);
	NVME_ADMIN_COMMAND *admin = admin_of(&cmd);

	admin->dword10 = NUMBER_OF_QUEUES;
	admin->dword11 = 0x00200010; /* NSQR=0x20, NCQR=0x10 */

	handle_nvme_admin_cmd(&cmd);

	expect_auto_cpl(4, ((MAX_NUM_OF_IO_SQ - 1) << 16) | (MAX_NUM_OF_IO_CQ - 1), 0);
}

static void test_set_features_volatile_write_cache_updates_context(void)
{
	NVME_COMMAND cmd = make_admin_cmd(ADMIN_SET_FEATURES, 5);
	NVME_ADMIN_COMMAND *admin = admin_of(&cmd);

	admin->dword10 = VOLATILE_WRITE_CACHE;
	admin->dword11 = 0x3; /* only bit 0 is WCE */
	g_nvmeTask.cacheEn = 0;

	handle_nvme_admin_cmd(&cmd);
	TEST_ASSERT_EQUAL_UINT(1, g_nvmeTask.cacheEn);
	expect_auto_cpl(5, 0, 0);

	mock_host_reset();
	admin->dword11 = 0x2;
	handle_nvme_admin_cmd(&cmd);
	TEST_ASSERT_EQUAL_UINT(0, g_nvmeTask.cacheEn);
	expect_auto_cpl(5, 0, 0);
}

static void set_features_no_op(unsigned char fid)
{
	NVME_COMMAND cmd = make_admin_cmd(ADMIN_SET_FEATURES, 6);
	NVME_ADMIN_COMMAND *admin = admin_of(&cmd);

	mock_host_reset();
	admin->dword10 = fid | (1u << 31); /* SV bit must be ignored */
	admin->dword11 = 0xDEADBEEF;

	handle_nvme_admin_cmd(&cmd);
	expect_auto_cpl(6, 0, 0);
}

static void test_set_features_accepted_no_op_fids(void)
{
	set_features_no_op(INTERRUPT_COALESCING);
	set_features_no_op(ARBITRATION);
	set_features_no_op(ASYNCHRONOUS_EVENT_CONFIGURATION);
	set_features_no_op(POWER_MANAGEMENT);
}

static void test_set_features_unsupported_fid_asserts(void)
{
	NVME_COMMAND cmd = make_admin_cmd(ADMIN_SET_FEATURES, 6);
	NVME_ADMIN_COMMAND *admin = admin_of(&cmd);

	admin->dword10 = LBA_RANGE_TYPE;
	FW_EXPECT_ASSERT(handle_nvme_admin_cmd(&cmd));

	admin->dword10 = TEMPERATURE_THRESHOLD;
	FW_EXPECT_ASSERT(handle_nvme_admin_cmd(&cmd));

	admin->dword10 = 0xFF;
	FW_EXPECT_ASSERT(handle_nvme_admin_cmd(&cmd));

	TEST_ASSERT_EQUAL_UINT(0, mock_host_call_count());
}

/* ------------------------------------------------------------------ */
/* Get Features                                                       */
/* ------------------------------------------------------------------ */

static void test_get_features_lba_range_type_returns_invalid_field(void)
{
	NVME_COMMAND cmd = make_admin_cmd(ADMIN_GET_FEATURES, 8);
	NVME_ADMIN_COMMAND *admin = admin_of(&cmd);
	NVME_COMPLETION expected;

	admin->dword10 = LBA_RANGE_TYPE;
	admin->NSID = 1;

	handle_nvme_admin_cmd(&cmd);

	expected.dword[0] = 0;
	expected.statusField.SC = SC_INVALID_FIELD_IN_COMMAND;
	TEST_ASSERT_EQUAL_HEX16(SC_INVALID_FIELD_IN_COMMAND << 1, expected.statusFieldWord);
	expect_auto_cpl(8, 0, expected.statusFieldWord);
}

static void test_get_features_lba_range_type_asserts_on_wrong_nsid(void)
{
	NVME_COMMAND cmd = make_admin_cmd(ADMIN_GET_FEATURES, 8);
	NVME_ADMIN_COMMAND *admin = admin_of(&cmd);

	admin->dword10 = LBA_RANGE_TYPE;
	admin->NSID = 2;

	FW_EXPECT_ASSERT(handle_nvme_admin_cmd(&cmd));
	TEST_ASSERT_EQUAL_UINT(0, mock_host_call_count());
}

static void test_get_features_temperature_threshold_echoes_dword11(void)
{
	NVME_COMMAND cmd = make_admin_cmd(ADMIN_GET_FEATURES, 9);
	NVME_ADMIN_COMMAND *admin = admin_of(&cmd);

	admin->dword10 = TEMPERATURE_THRESHOLD;
	admin->dword11 = 0x0000015E;

	handle_nvme_admin_cmd(&cmd);
	expect_auto_cpl(9, 0x0000015E, 0);
}

static void test_get_features_volatile_write_cache_reports_context(void)
{
	NVME_COMMAND cmd = make_admin_cmd(ADMIN_GET_FEATURES, 10);
	NVME_ADMIN_COMMAND *admin = admin_of(&cmd);

	admin->dword10 = VOLATILE_WRITE_CACHE;

	g_nvmeTask.cacheEn = 1;
	handle_nvme_admin_cmd(&cmd);
	expect_auto_cpl(10, 1, 0);

	mock_host_reset();
	g_nvmeTask.cacheEn = 0;
	handle_nvme_admin_cmd(&cmd);
	expect_auto_cpl(10, 0, 0);
}

static void test_get_features_power_management_returns_zero(void)
{
	NVME_COMMAND cmd = make_admin_cmd(ADMIN_GET_FEATURES, 11);
	NVME_ADMIN_COMMAND *admin = admin_of(&cmd);

	admin->dword10 = POWER_MANAGEMENT | (0x7 << 8); /* SEL bits ignored */
	admin->dword11 = 0x55;

	handle_nvme_admin_cmd(&cmd);
	expect_auto_cpl(11, 0, 0);
}

static void test_get_features_unsupported_fid_asserts(void)
{
	NVME_COMMAND cmd = make_admin_cmd(ADMIN_GET_FEATURES, 11);
	NVME_ADMIN_COMMAND *admin = admin_of(&cmd);

	admin->dword10 = NUMBER_OF_QUEUES;
	FW_EXPECT_ASSERT(handle_nvme_admin_cmd(&cmd));

	admin->dword10 = ARBITRATION;
	FW_EXPECT_ASSERT(handle_nvme_admin_cmd(&cmd));

	admin->dword10 = INTERRUPT_COALESCING;
	FW_EXPECT_ASSERT(handle_nvme_admin_cmd(&cmd));

	TEST_ASSERT_EQUAL_UINT(0, mock_host_call_count());
}

/* ------------------------------------------------------------------ */
/* Create / Delete I/O queues                                         */
/* ------------------------------------------------------------------ */

static void test_create_io_sq_decodes_fields_and_programs_hw(void)
{
	NVME_COMMAND cmd = make_admin_cmd(ADMIN_CREATE_IO_SQ, 12);
	NVME_ADMIN_COMMAND *admin = admin_of(&cmd);
	const mock_host_call_t *sq;

	admin->PRP1[0] = 0xABCDE000;
	admin->PRP1[1] = 0xF;
	admin->dword10 = (0x7F << 16) | 3;             /* QSIZE=0x7F, QID=3 */
	admin->dword11 = (5 << 16) | (0x3 << 1) | 0x1; /* CQID=5, QPRIO=3, PC=1 */

	handle_nvme_admin_cmd(&cmd);

	TEST_ASSERT_EQUAL_UINT(1, g_nvmeTask.ioSqInfo[2].valid);
	TEST_ASSERT_EQUAL_UINT(0x7F, g_nvmeTask.ioSqInfo[2].qSzie);
	TEST_ASSERT_EQUAL_UINT(5, g_nvmeTask.ioSqInfo[2].cqVector);
	TEST_ASSERT_EQUAL_HEX32(0xABCDE000, g_nvmeTask.ioSqInfo[2].pcieBaseAddrL);
	TEST_ASSERT_EQUAL_HEX32(0xF, g_nvmeTask.ioSqInfo[2].pcieBaseAddrH);
	TEST_ASSERT_EQUAL_UINT(0, g_nvmeTask.ioSqInfo[1].valid);
	TEST_ASSERT_EQUAL_UINT(0, g_nvmeTask.ioSqInfo[3].valid);

	TEST_ASSERT_EQUAL_UINT(1, mock_host_count(MOCK_HOST_SET_IO_SQ));
	sq = mock_host_last(MOCK_HOST_SET_IO_SQ);
	TEST_ASSERT_EQUAL_UINT(2, sq->args[0]);
	TEST_ASSERT_EQUAL_UINT(1, sq->args[1]);
	TEST_ASSERT_EQUAL_UINT(5, sq->args[2]);
	TEST_ASSERT_EQUAL_UINT(0x7F, sq->args[3]);
	TEST_ASSERT_EQUAL_HEX32(0xABCDE000, sq->args[4]);
	TEST_ASSERT_EQUAL_HEX32(0xF, sq->args[5]);
	expect_auto_cpl(12, 0, 0);
}

static void test_create_io_sq_boundary_qids(void)
{
	NVME_COMMAND cmd = make_admin_cmd(ADMIN_CREATE_IO_SQ, 12);
	NVME_ADMIN_COMMAND *admin = admin_of(&cmd);

	admin->PRP1[0] = 0x10000;
	admin->dword10 = (0xFF << 16) | 1;
	admin->dword11 = (1 << 16);
	handle_nvme_admin_cmd(&cmd);
	TEST_ASSERT_EQUAL_UINT(1, g_nvmeTask.ioSqInfo[0].valid);
	TEST_ASSERT_EQUAL_UINT(0xFF, g_nvmeTask.ioSqInfo[0].qSzie);

	admin->dword10 = (0x10 << 16) | MAX_NUM_OF_IO_SQ;
	admin->dword11 = (MAX_NUM_OF_IO_CQ << 16);
	handle_nvme_admin_cmd(&cmd);
	TEST_ASSERT_EQUAL_UINT(1, g_nvmeTask.ioSqInfo[MAX_NUM_OF_IO_SQ - 1].valid);
	TEST_ASSERT_EQUAL_UINT(MAX_NUM_OF_IO_CQ, g_nvmeTask.ioSqInfo[MAX_NUM_OF_IO_SQ - 1].cqVector);
	TEST_ASSERT_EQUAL_UINT(2, mock_host_count(MOCK_HOST_SET_IO_SQ));
	TEST_ASSERT_EQUAL_UINT(2, mock_host_count(MOCK_HOST_SET_AUTO_NVME_CPL));
}

static void test_create_io_sq_rejects_invalid_parameters(void)
{
	NVME_COMMAND cmd = make_admin_cmd(ADMIN_CREATE_IO_SQ, 12);
	NVME_ADMIN_COMMAND *admin = admin_of(&cmd);

	/* QID 0 */
	admin->PRP1[0] = 0x10000;
	admin->dword10 = (0x10 << 16) | 0;
	admin->dword11 = (1 << 16);
	FW_EXPECT_ASSERT(handle_nvme_admin_cmd(&cmd));

	/* QID > 8 */
	admin->dword10 = (0x10 << 16) | 9;
	FW_EXPECT_ASSERT(handle_nvme_admin_cmd(&cmd));

	/* QSIZE >= 0x100 */
	admin->dword10 = (0x100 << 16) | 1;
	FW_EXPECT_ASSERT(handle_nvme_admin_cmd(&cmd));

	/* CQID 0 */
	admin->dword10 = (0x10 << 16) | 1;
	admin->dword11 = 0;
	FW_EXPECT_ASSERT(handle_nvme_admin_cmd(&cmd));

	/* CQID > 8 */
	admin->dword11 = (9 << 16);
	FW_EXPECT_ASSERT(handle_nvme_admin_cmd(&cmd));

	/* misaligned PRP1 low */
	admin->dword11 = (1 << 16);
	admin->PRP1[0] = 0x10008;
	FW_EXPECT_ASSERT(handle_nvme_admin_cmd(&cmd));

	/* PRP1 high out of range */
	admin->PRP1[0] = 0x10000;
	admin->PRP1[1] = 0x10;
	FW_EXPECT_ASSERT(handle_nvme_admin_cmd(&cmd));

	TEST_ASSERT_EQUAL_UINT(0, mock_host_call_count());
	TEST_ASSERT_EQUAL_UINT(0, g_nvmeTask.ioSqInfo[0].valid);
}

static void test_delete_io_sq_clears_state_and_programs_hw(void)
{
	NVME_COMMAND cmd = make_admin_cmd(ADMIN_CREATE_IO_SQ, 13);
	NVME_ADMIN_COMMAND *admin = admin_of(&cmd);
	const mock_host_call_t *sq;

	admin->PRP1[0] = 0x20000;
	admin->PRP1[1] = 0x1;
	admin->dword10 = (0x20 << 16) | 4;
	admin->dword11 = (2 << 16);
	handle_nvme_admin_cmd(&cmd);
	TEST_ASSERT_EQUAL_UINT(1, g_nvmeTask.ioSqInfo[3].valid);

	mock_host_reset();
	cmd = make_admin_cmd(ADMIN_DELETE_IO_SQ, 14);
	admin = admin_of(&cmd);
	admin->dword10 = 0xFFFF0000 | 4; /* reserved upper half ignored */
	handle_nvme_admin_cmd(&cmd);

	TEST_ASSERT_EQUAL_UINT(0, g_nvmeTask.ioSqInfo[3].valid);
	TEST_ASSERT_EQUAL_UINT(0, g_nvmeTask.ioSqInfo[3].cqVector);
	TEST_ASSERT_EQUAL_UINT(0, g_nvmeTask.ioSqInfo[3].qSzie);
	TEST_ASSERT_EQUAL_HEX32(0, g_nvmeTask.ioSqInfo[3].pcieBaseAddrL);
	TEST_ASSERT_EQUAL_HEX32(0, g_nvmeTask.ioSqInfo[3].pcieBaseAddrH);

	TEST_ASSERT_EQUAL_UINT(1, mock_host_count(MOCK_HOST_SET_IO_SQ));
	sq = mock_host_last(MOCK_HOST_SET_IO_SQ);
	TEST_ASSERT_EQUAL_UINT(3, sq->args[0]);
	TEST_ASSERT_EQUAL_UINT(0, sq->args[1]);
	TEST_ASSERT_EQUAL_UINT(0, sq->args[2]);
	TEST_ASSERT_EQUAL_UINT(0, sq->args[3]);
	TEST_ASSERT_EQUAL_UINT(0, sq->args[4]);
	TEST_ASSERT_EQUAL_UINT(0, sq->args[5]);
	expect_auto_cpl(14, 0, 0);
}

static void test_create_io_cq_decodes_fields_and_programs_hw(void)
{
	NVME_COMMAND cmd = make_admin_cmd(ADMIN_CREATE_IO_CQ, 15);
	NVME_ADMIN_COMMAND *admin = admin_of(&cmd);
	const mock_host_call_t *cq;

	admin->PRP1[0] = 0x12345000;
	admin->PRP1[1] = 0x0;
	admin->dword10 = (0x3F << 16) | 6;    /* QSIZE=0x3F, QID=6 */
	admin->dword11 = (7 << 16) | 0x2 | 0x1; /* IV=7, IEN=1, PC=1 */

	handle_nvme_admin_cmd(&cmd);

	TEST_ASSERT_EQUAL_UINT(1, g_nvmeTask.ioCqInfo[5].valid);
	TEST_ASSERT_EQUAL_UINT(0x3F, g_nvmeTask.ioCqInfo[5].qSzie);
	TEST_ASSERT_EQUAL_UINT(1, g_nvmeTask.ioCqInfo[5].irqEn);
	TEST_ASSERT_EQUAL_UINT(7, g_nvmeTask.ioCqInfo[5].irqVector);
	TEST_ASSERT_EQUAL_HEX32(0x12345000, g_nvmeTask.ioCqInfo[5].pcieBaseAddrL);
	TEST_ASSERT_EQUAL_HEX32(0, g_nvmeTask.ioCqInfo[5].pcieBaseAddrH);

	TEST_ASSERT_EQUAL_UINT(1, mock_host_count(MOCK_HOST_SET_IO_CQ));
	cq = mock_host_last(MOCK_HOST_SET_IO_CQ);
	TEST_ASSERT_EQUAL_UINT(5, cq->args[0]);
	TEST_ASSERT_EQUAL_UINT(1, cq->args[1]);
	TEST_ASSERT_EQUAL_UINT(1, cq->args[2]);
	TEST_ASSERT_EQUAL_UINT(7, cq->args[3]);
	TEST_ASSERT_EQUAL_UINT(0x3F, cq->args[4]);
	TEST_ASSERT_EQUAL_HEX32(0x12345000, cq->args[5]);
	TEST_ASSERT_EQUAL_HEX32(0, cq->args[6]);
	expect_auto_cpl(15, 0, 0);
}

static void test_create_io_cq_without_interrupts(void)
{
	NVME_COMMAND cmd = make_admin_cmd(ADMIN_CREATE_IO_CQ, 15);
	NVME_ADMIN_COMMAND *admin = admin_of(&cmd);

	admin->PRP1[0] = 0x10000;
	admin->dword10 = (0x10 << 16) | 1;
	admin->dword11 = 0x1; /* PC only, IEN=0, IV=0 */

	handle_nvme_admin_cmd(&cmd);

	TEST_ASSERT_EQUAL_UINT(1, g_nvmeTask.ioCqInfo[0].valid);
	TEST_ASSERT_EQUAL_UINT(0, g_nvmeTask.ioCqInfo[0].irqEn);
	TEST_ASSERT_EQUAL_UINT(0, g_nvmeTask.ioCqInfo[0].irqVector);
	TEST_ASSERT_EQUAL_UINT(0, mock_host_last(MOCK_HOST_SET_IO_CQ)->args[2]);
}

static void test_create_io_cq_rejects_invalid_parameters(void)
{
	NVME_COMMAND cmd = make_admin_cmd(ADMIN_CREATE_IO_CQ, 15);
	NVME_ADMIN_COMMAND *admin = admin_of(&cmd);

	/* IV >= 8 */
	admin->PRP1[0] = 0x10000;
	admin->dword10 = (0x10 << 16) | 1;
	admin->dword11 = (8 << 16) | 0x2;
	FW_EXPECT_ASSERT(handle_nvme_admin_cmd(&cmd));

	/* QSIZE >= 0x100 */
	admin->dword11 = 0x1;
	admin->dword10 = (0x100 << 16) | 1;
	FW_EXPECT_ASSERT(handle_nvme_admin_cmd(&cmd));

	/* QID 0 */
	admin->dword10 = (0x10 << 16) | 0;
	FW_EXPECT_ASSERT(handle_nvme_admin_cmd(&cmd));

	/* QID > 8 */
	admin->dword10 = (0x10 << 16) | 9;
	FW_EXPECT_ASSERT(handle_nvme_admin_cmd(&cmd));

	/* misaligned PRP1 */
	admin->dword10 = (0x10 << 16) | 1;
	admin->PRP1[0] = 0x10001;
	FW_EXPECT_ASSERT(handle_nvme_admin_cmd(&cmd));

	/* PRP1 high out of range */
	admin->PRP1[0] = 0x10000;
	admin->PRP1[1] = 0x10;
	FW_EXPECT_ASSERT(handle_nvme_admin_cmd(&cmd));

	TEST_ASSERT_EQUAL_UINT(0, mock_host_call_count());
	TEST_ASSERT_EQUAL_UINT(0, g_nvmeTask.ioCqInfo[0].valid);
}

static void test_delete_io_cq_clears_state_and_programs_hw(void)
{
	NVME_COMMAND cmd = make_admin_cmd(ADMIN_CREATE_IO_CQ, 16);
	NVME_ADMIN_COMMAND *admin = admin_of(&cmd);
	const mock_host_call_t *cq;

	admin->PRP1[0] = 0x30000;
	admin->PRP1[1] = 0x2;
	admin->dword10 = (0x40 << 16) | 8;
	admin->dword11 = (3 << 16) | 0x3;
	handle_nvme_admin_cmd(&cmd);
	TEST_ASSERT_EQUAL_UINT(1, g_nvmeTask.ioCqInfo[7].valid);

	mock_host_reset();
	cmd = make_admin_cmd(ADMIN_DELETE_IO_CQ, 17);
	admin = admin_of(&cmd);
	admin->dword10 = 8;
	handle_nvme_admin_cmd(&cmd);

	TEST_ASSERT_EQUAL_UINT(0, g_nvmeTask.ioCqInfo[7].valid);
	TEST_ASSERT_EQUAL_UINT(0, g_nvmeTask.ioCqInfo[7].irqVector);
	TEST_ASSERT_EQUAL_UINT(0, g_nvmeTask.ioCqInfo[7].qSzie);
	TEST_ASSERT_EQUAL_HEX32(0, g_nvmeTask.ioCqInfo[7].pcieBaseAddrL);
	TEST_ASSERT_EQUAL_HEX32(0, g_nvmeTask.ioCqInfo[7].pcieBaseAddrH);

	TEST_ASSERT_EQUAL_UINT(1, mock_host_count(MOCK_HOST_SET_IO_CQ));
	cq = mock_host_last(MOCK_HOST_SET_IO_CQ);
	TEST_ASSERT_EQUAL_UINT(7, cq->args[0]);
	TEST_ASSERT_EQUAL_UINT(0, cq->args[1]);
	TEST_ASSERT_EQUAL_UINT(0, cq->args[2]);
	TEST_ASSERT_EQUAL_UINT(0, cq->args[3]);
	TEST_ASSERT_EQUAL_UINT(0, cq->args[4]);
	TEST_ASSERT_EQUAL_UINT(0, cq->args[5]);
	TEST_ASSERT_EQUAL_UINT(0, cq->args[6]);
	expect_auto_cpl(17, 0, 0);
}

static void test_delete_io_cq_leaves_irq_enable_untouched(void)
{
	/* handle_delete_io_cq() resets every field except irqEn. Documented as
	 * observed behaviour so a future change is deliberate. */
	NVME_COMMAND cmd = make_admin_cmd(ADMIN_DELETE_IO_CQ, 17);
	NVME_ADMIN_COMMAND *admin = admin_of(&cmd);

	g_nvmeTask.ioCqInfo[1].valid = 1;
	g_nvmeTask.ioCqInfo[1].irqEn = 1;
	g_nvmeTask.ioCqInfo[1].irqVector = 2;
	admin->dword10 = 2;

	handle_nvme_admin_cmd(&cmd);

	TEST_ASSERT_EQUAL_UINT(0, g_nvmeTask.ioCqInfo[1].valid);
	TEST_ASSERT_EQUAL_UINT(0, g_nvmeTask.ioCqInfo[1].irqVector);
	TEST_ASSERT_EQUAL_UINT(1, g_nvmeTask.ioCqInfo[1].irqEn);
}

/* ------------------------------------------------------------------ */
/* Other opcodes                                                      */
/* ------------------------------------------------------------------ */

static void test_async_event_request_releases_slot_without_completion(void)
{
	NVME_COMMAND cmd = make_admin_cmd(ADMIN_ASYNCHRONOUS_EVENT_REQUEST, 18);

	handle_nvme_admin_cmd(&cmd);

	TEST_ASSERT_EQUAL_UINT(1, mock_host_count(MOCK_HOST_SET_NVME_SLOT_RELEASE));
	TEST_ASSERT_EQUAL_UINT(18, mock_host_last(MOCK_HOST_SET_NVME_SLOT_RELEASE)->args[0]);
	TEST_ASSERT_EQUAL_UINT(0, mock_host_count(MOCK_HOST_SET_AUTO_NVME_CPL));
	TEST_ASSERT_EQUAL_UINT(0, mock_host_count(MOCK_HOST_SET_NVME_CPL));
	TEST_ASSERT_EQUAL_UINT(1, mock_host_call_count());
}

static void test_get_log_page_reports_invalid_log_page(void)
{
	NVME_COMMAND cmd = make_admin_cmd(ADMIN_GET_LOG_PAGE, 19);
	NVME_ADMIN_COMMAND *admin = admin_of(&cmd);

	admin->dword10 = 0x00FF0002; /* LID = SMART/Health, NUMD = 0xFF */
	admin->PRP1[0] = 0x1000;

	handle_nvme_admin_cmd(&cmd);

	expect_auto_cpl(19, 0x9, 0);
	TEST_ASSERT_EQUAL_UINT(0, mock_host_count(MOCK_HOST_SET_DIRECT_TX_DMA));
}

static void unsupported_opcode_asserts(unsigned char opc)
{
	NVME_COMMAND cmd = make_admin_cmd(opc, 20);

	FW_EXPECT_ASSERT(handle_nvme_admin_cmd(&cmd));
}

static void test_unsupported_opcodes_assert_without_completion(void)
{
	unsupported_opcode_asserts(ADMIN_ABORT);
	unsupported_opcode_asserts(ADMIN_FIRMWARE_ACTIVATE);
	unsupported_opcode_asserts(ADMIN_FIRMWARE_IMAGE_DOWNLOAD);
	unsupported_opcode_asserts(ADMIN_FORMAT_NVM);
	unsupported_opcode_asserts(ADMIN_SECURITY_SEND);
	unsupported_opcode_asserts(ADMIN_SECURITY_RECEIVE);
	unsupported_opcode_asserts(0x03);
	unsupported_opcode_asserts(0xFF);

	TEST_ASSERT_EQUAL_UINT(8, fw_assert_count());
	TEST_ASSERT_EQUAL_UINT(0, mock_host_call_count());
}

static void test_completion_uses_command_slot_tag(void)
{
	NVME_COMMAND cmd = make_admin_cmd(ADMIN_GET_FEATURES, 0x3FF);
	NVME_ADMIN_COMMAND *admin = admin_of(&cmd);

	admin->dword10 = POWER_MANAGEMENT;
	cmd.qID = 0;
	cmd.cmdSeqNum = 77;

	handle_nvme_admin_cmd(&cmd);
	expect_auto_cpl(0x3FF, 0, 0);
}

int main(void)
{
	UNITY_BEGIN();
	RUN_TEST(test_smoke_identify_controller_fills_buffer_and_completes);
	RUN_TEST(test_identify_namespace_fills_buffer_and_completes);
	RUN_TEST(test_identify_ignores_upper_cns_bits);
	RUN_TEST(test_identify_split_prp_issues_two_dmas);
	RUN_TEST(test_identify_split_prp_asserts_on_misaligned_prp2_high);
	RUN_TEST(test_identify_controller_asserts_on_misaligned_prp1);
	RUN_TEST(test_identify_namespace_asserts_on_misaligned_prp2);
	RUN_TEST(test_identify_controller_data_fields);
	RUN_TEST(test_identify_namespace_data_fields);
	RUN_TEST(test_identify_namespace_tracks_ftl_capacity);
	RUN_TEST(test_get_num_of_queue_passes_small_counts_through);
	RUN_TEST(test_get_num_of_queue_clamps_to_max);
	RUN_TEST(test_set_features_number_of_queues_reports_clamped_count);
	RUN_TEST(test_set_features_volatile_write_cache_updates_context);
	RUN_TEST(test_set_features_accepted_no_op_fids);
	RUN_TEST(test_set_features_unsupported_fid_asserts);
	RUN_TEST(test_get_features_lba_range_type_returns_invalid_field);
	RUN_TEST(test_get_features_lba_range_type_asserts_on_wrong_nsid);
	RUN_TEST(test_get_features_temperature_threshold_echoes_dword11);
	RUN_TEST(test_get_features_volatile_write_cache_reports_context);
	RUN_TEST(test_get_features_power_management_returns_zero);
	RUN_TEST(test_get_features_unsupported_fid_asserts);
	RUN_TEST(test_create_io_sq_decodes_fields_and_programs_hw);
	RUN_TEST(test_create_io_sq_boundary_qids);
	RUN_TEST(test_create_io_sq_rejects_invalid_parameters);
	RUN_TEST(test_delete_io_sq_clears_state_and_programs_hw);
	RUN_TEST(test_create_io_cq_decodes_fields_and_programs_hw);
	RUN_TEST(test_create_io_cq_without_interrupts);
	RUN_TEST(test_create_io_cq_rejects_invalid_parameters);
	RUN_TEST(test_delete_io_cq_clears_state_and_programs_hw);
	RUN_TEST(test_delete_io_cq_leaves_irq_enable_untouched);
	RUN_TEST(test_async_event_request_releases_slot_without_completion);
	RUN_TEST(test_get_log_page_reports_invalid_log_page);
	RUN_TEST(test_unsupported_opcodes_assert_without_completion);
	RUN_TEST(test_completion_uses_command_slot_tag);
	return UNITY_END();
}
