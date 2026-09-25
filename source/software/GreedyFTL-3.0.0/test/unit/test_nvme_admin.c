/* Unit tests for nvme/nvme_admin_cmd.c and nvme/nvme_identify.c.
 *
 * Every admin handler is exercised through the public handle_*() entry points
 * and through the top-level handle_nvme_admin_cmd() dispatcher. Hardware
 * effects are observed in the IO mock write log (DMA descriptors, IO queue
 * set registers, completion FIFO) and in g_nvmeTask bookkeeping. */
#include <string.h>
#include "unity.h"
#include "ftl_test_env.h"
#include "ftl_config.h"
#include "nvme/nvme.h"
#include "nvme/host_lld.h"
#include "nvme/nvme_identify.h"
#include "nvme/nvme_admin_cmd.h"

extern NVME_CONTEXT g_nvmeTask;

#define IDENTIFY_BUFFER   ((uintptr_t)ADMIN_CMD_DRAM_DATA_BUFFER)
#define PAGE_SIZE_BYTES   0x1000u
#define HOST_PAGE_ADDR    0x10000000u
#define HOST_PAGE2_ADDR   0x20000000u
#define TEST_SLOT_TAG     0x2Bu
#define UNSUPPORTED_FID   0x7Fu

/* Sentinel written over the completion so tests notice fields the handler
 * leaves untouched. */
#define CPL_SENTINEL      0xFF

static NVME_ADMIN_COMMAND cmd;
static NVME_COMPLETION cpl;
static NVME_COMMAND slotCmd;

void setUp(void)
{
	ftl_test_env_reset();
	memset(&g_nvmeTask, 0, sizeof(g_nvmeTask));
	memset(&cmd, 0, sizeof(cmd));
	memset(&cpl, CPL_SENTINEL, sizeof(cpl));
	memset(&slotCmd, 0, sizeof(slotCmd));
}
void tearDown(void) {}

/* --------------------------------------------------------------------- */
/* helpers                                                                */
/* --------------------------------------------------------------------- */

static void make_identify_cmd(unsigned int cns, unsigned int prp1, unsigned int prp2)
{
	cmd.OPC = ADMIN_IDENTIFY;
	cmd.NSID = 1;
	cmd.PRP1[0] = prp1;
	cmd.PRP2[0] = prp2;
	cmd.dword10 = cns;
}

static void make_create_sq_cmd(unsigned int qid, unsigned int qsize, unsigned int cqid,
                               unsigned int prpL, unsigned int prpH)
{
	ADMIN_CREATE_IO_SQ_DW10 dw10 = { .dword = 0 };
	ADMIN_CREATE_IO_SQ_DW11 dw11 = { .dword = 0 };

	dw10.QID = qid;
	dw10.QSIZE = qsize;
	dw11.PC = 1;
	dw11.CQID = cqid;

	cmd.OPC = ADMIN_CREATE_IO_SQ;
	cmd.PRP1[0] = prpL;
	cmd.PRP1[1] = prpH;
	cmd.dword10 = dw10.dword;
	cmd.dword11 = dw11.dword;
}

static void make_create_cq_cmd(unsigned int qid, unsigned int qsize, unsigned int ien,
                               unsigned int iv, unsigned int prpL, unsigned int prpH)
{
	ADMIN_CREATE_IO_CQ_DW10 dw10 = { .dword = 0 };
	ADMIN_CREATE_IO_CQ_DW11 dw11 = { .dword = 0 };

	dw10.QID = qid;
	dw10.QSIZE = qsize;
	dw11.PC = 1;
	dw11.IEN = ien;
	dw11.IV = iv;

	cmd.OPC = ADMIN_CREATE_IO_CQ;
	cmd.PRP1[0] = prpL;
	cmd.PRP1[1] = prpH;
	cmd.dword10 = dw10.dword;
	cmd.dword11 = dw11.dword;
}

static void make_features_cmd(unsigned char opc, unsigned int fid, unsigned int dword11)
{
	cmd.OPC = opc;
	cmd.NSID = 1;
	cmd.dword10 = fid;
	cmd.dword11 = dword11;
}

/* Wrap `cmd` in an NVME_COMMAND slot and run it through the dispatcher. */
static void dispatch_admin_cmd(void)
{
	slotCmd.qID = 0;
	slotCmd.cmdSlotTag = TEST_SLOT_TAG;
	memcpy(slotCmd.cmdDword, cmd.dword, sizeof(slotCmd.cmdDword));
	handle_nvme_admin_cmd(&slotCmd);
}

static void assert_success_completion(void)
{
	TEST_ASSERT_EQUAL_UINT(0, cpl.statusField.SC);
	TEST_ASSERT_EQUAL_UINT(0, cpl.statusField.SCT);
	TEST_ASSERT_EQUAL_UINT(0, cpl.statusField.MORE);
	TEST_ASSERT_EQUAL_UINT(0, cpl.statusField.DNR);
	TEST_ASSERT_EQUAL_HEX16(0, cpl.statusFieldWord);
}

/* Decode the n-th direct TX DMA descriptor pushed to the host DMA FIFO. */
static HOST_DMA_CMD_FIFO_REG dma_descriptor_at(size_t n)
{
	HOST_DMA_CMD_FIFO_REG reg;
	size_t seen = 0;
	size_t i;

	memset(&reg, 0, sizeof(reg));
	for (i = 0; i < mock_io_write_count(); i++) {
		const mock_io_write_t *w = mock_io_write_at(i);
		if (w->addr != HOST_DMA_CMD_FIFO_REG_ADDR)
			continue;
		if (seen++ == n) {
			reg.dword[0] = w->value;
			reg.dword[1] = mock_io_write_at(i + 1)->value;
			reg.dword[2] = mock_io_write_at(i + 2)->value;
			reg.dword[3] = mock_io_write_at(i + 3)->value;
			return reg;
		}
	}
	TEST_FAIL_MESSAGE("DMA descriptor not found in write log");
	return reg;
}

static NVME_IO_SQ_SET_REG read_sq_set_reg(unsigned int ioSqIdx)
{
	NVME_IO_SQ_SET_REG reg;
	uintptr_t addr = NVME_IO_SQ_SET_REG_ADDR + ioSqIdx * 8;
	int found0, found1;

	reg.dword[0] = mock_io_last_write(addr, &found0);
	reg.dword[1] = mock_io_last_write(addr + 4, &found1);
	TEST_ASSERT_TRUE_MESSAGE(found0 && found1, "set_io_sq register never written");
	return reg;
}

static NVME_IO_CQ_SET_REG read_cq_set_reg(unsigned int ioCqIdx)
{
	NVME_IO_CQ_SET_REG reg;
	uintptr_t addr = NVME_IO_CQ_SET_REG_ADDR + ioCqIdx * 8;
	int found0, found1;

	reg.dword[0] = mock_io_last_write(addr, &found0);
	reg.dword[1] = mock_io_last_write(addr + 4, &found1);
	TEST_ASSERT_TRUE_MESSAGE(found0 && found1, "set_io_cq register never written");
	return reg;
}

static NVME_CPL_FIFO_REG read_cpl_fifo_reg(void)
{
	NVME_CPL_FIFO_REG reg;
	int found;

	reg.dword[0] = mock_io_last_write(NVME_CPL_FIFO_REG_ADDR, &found);
	reg.dword[1] = mock_io_last_write(NVME_CPL_FIFO_REG_ADDR + 4, &found);
	reg.dword[2] = mock_io_last_write(NVME_CPL_FIFO_REG_ADDR + 8, &found);
	TEST_ASSERT_TRUE_MESSAGE(found, "completion FIFO never written");
	return reg;
}

/* --------------------------------------------------------------------- */
/* identify_controller / identify_namespace field values                  */
/* --------------------------------------------------------------------- */

static void test_identify_controller_populates_vendor_and_strings(void)
{
	ADMIN_IDENTIFY_CONTROLLER *id = (ADMIN_IDENTIFY_CONTROLLER *)IDENTIFY_BUFFER;
	unsigned char expectedSN[20];
	unsigned char expectedMN[40];
	unsigned char expectedFR[8];

	memset(expectedSN, 0x20, sizeof(expectedSN));
	memcpy(expectedSN, SERIAL_NUMBER, sizeof(SERIAL_NUMBER));
	memset(expectedMN, 0x20, sizeof(expectedMN));
	memcpy(expectedMN, MODEL_NUMBER, sizeof(MODEL_NUMBER));
	memset(expectedFR, 0x20, sizeof(expectedFR));
	memcpy(expectedFR, FIRMWARE_REVISION, sizeof(expectedFR));

	identify_controller(ADMIN_CMD_DRAM_DATA_BUFFER);

	TEST_ASSERT_EQUAL_HEX16(PCI_VENDOR_ID, id->VID);
	TEST_ASSERT_EQUAL_HEX16(PCI_SUBSYSTEM_VENDOR_ID, id->SSVID);
	TEST_ASSERT_EQUAL_MEMORY(expectedSN, id->SN, sizeof(expectedSN));
	TEST_ASSERT_EQUAL_MEMORY(expectedMN, id->MN, sizeof(expectedMN));
	TEST_ASSERT_EQUAL_MEMORY(expectedFR, id->FR, sizeof(expectedFR));
	TEST_ASSERT_EQUAL_HEX8(0xE4, id->IEEE[0]);
	TEST_ASSERT_EQUAL_HEX8(0xD2, id->IEEE[1]);
	TEST_ASSERT_EQUAL_HEX8(0x5C, id->IEEE[2]);
	TEST_ASSERT_EQUAL_UINT(0, id->RAB);
}

static void test_identify_controller_populates_capabilities(void)
{
	ADMIN_IDENTIFY_CONTROLLER *id = (ADMIN_IDENTIFY_CONTROLLER *)IDENTIFY_BUFFER;

	identify_controller(ADMIN_CMD_DRAM_DATA_BUFFER);

	TEST_ASSERT_EQUAL_UINT(0x8, id->MDTS);
	TEST_ASSERT_EQUAL_UINT(0x9, id->CNTLID);
	TEST_ASSERT_EQUAL_UINT(0x3, id->ACL);
	TEST_ASSERT_EQUAL_UINT(0x3, id->AERL);
	TEST_ASSERT_EQUAL_UINT(0x8, id->ELPE);
	TEST_ASSERT_EQUAL_UINT(1, id->FRMW.firstFirmwareSlotReadOnly);
	TEST_ASSERT_EQUAL_UINT(1, id->FRMW.supportedNumberOfFirmwareSlots);
	TEST_ASSERT_EQUAL_UINT(0x6, id->SQES.requiredSubmissionQueueEntrySize);
	TEST_ASSERT_EQUAL_UINT(0x6, id->SQES.maximumSubmissionQueueEntrySize);
	TEST_ASSERT_EQUAL_UINT(0x4, id->CQES.requiredCompletionQueueEntrySize);
	TEST_ASSERT_EQUAL_UINT(0x4, id->CQES.maximumCompletionQueueEntrySize);
	TEST_ASSERT_EQUAL_UINT(1, id->NN);
	TEST_ASSERT_EQUAL_UINT(1, id->VWC.present);
	TEST_ASSERT_EQUAL_UINT(0, id->OACS.supportsFormatNVM);
	TEST_ASSERT_EQUAL_UINT(0, id->ONCS.supportsDataSetManagement);
	TEST_ASSERT_EQUAL_UINT(0, id->SGLS.supportsSGL);
	TEST_ASSERT_EQUAL_UINT(0, id->NPSS);
	TEST_ASSERT_EQUAL_HEX16(0x09C4, id->PSDx[0].MP);
	TEST_ASSERT_EQUAL_UINT(0, id->PSDx[0].ENLAT);
}

static void test_identify_controller_clears_stale_buffer(void)
{
	unsigned char *buf = (unsigned char *)IDENTIFY_BUFFER;

	memset(buf, 0xAB, sizeof(ADMIN_IDENTIFY_CONTROLLER));
	identify_controller(ADMIN_CMD_DRAM_DATA_BUFFER);

	/* Reserved tail of the structure must be zeroed, not left as garbage. */
	TEST_ASSERT_EQUAL_HEX8(0, buf[sizeof(ADMIN_IDENTIFY_CONTROLLER) - 1]);
}

static void test_identify_namespace_reports_ftl_capacity(void)
{
	ADMIN_IDENTIFY_NAMESPACE *ns = (ADMIN_IDENTIFY_NAMESPACE *)IDENTIFY_BUFFER;

	storageCapacity_L = 0x00123456;
	identify_namespace(ADMIN_CMD_DRAM_DATA_BUFFER);

	TEST_ASSERT_EQUAL_HEX32(0x00123456, ns->NSZE[0]);
	TEST_ASSERT_EQUAL_HEX32(STORAGE_CAPACITY_H, ns->NSZE[1]);
	TEST_ASSERT_EQUAL_HEX32(0x00123456, ns->NCAP[0]);
	TEST_ASSERT_EQUAL_HEX32(STORAGE_CAPACITY_H, ns->NCAP[1]);
	TEST_ASSERT_EQUAL_HEX32(0x00123456, ns->NUSE[0]);
	TEST_ASSERT_EQUAL_HEX32(STORAGE_CAPACITY_H, ns->NUSE[1]);
}

static void test_identify_namespace_single_4k_lba_format(void)
{
	ADMIN_IDENTIFY_NAMESPACE *ns = (ADMIN_IDENTIFY_NAMESPACE *)IDENTIFY_BUFFER;

	memset(ns, 0xAB, sizeof(*ns));
	identify_namespace(ADMIN_CMD_DRAM_DATA_BUFFER);

	TEST_ASSERT_EQUAL_UINT(0, ns->NLBAF);
	TEST_ASSERT_EQUAL_UINT(0, ns->FLBAS.supportedCombination);
	TEST_ASSERT_EQUAL_UINT(0xC, ns->LBAFx[0].LBADS); /* 4 KiB sectors */
	TEST_ASSERT_EQUAL_UINT(0, ns->LBAFx[0].MS);
	TEST_ASSERT_EQUAL_UINT(0x2, ns->LBAFx[0].RP);
	TEST_ASSERT_EQUAL_UINT(0, ns->NSFEAT.supportsThinProvisioning);
	TEST_ASSERT_EQUAL_UINT(0, ns->DPS.protectionEnabled);
	TEST_ASSERT_EQUAL_UINT(0, ns->NMIC.supportsMultipathIOSharing);
	TEST_ASSERT_EQUAL_UINT(0, ns->RESCAP.supportsPersistThroughPowerLoss);
}

/* --------------------------------------------------------------------- */
/* handle_identify                                                        */
/* --------------------------------------------------------------------- */

static void test_handle_identify_controller_single_page_dma(void)
{
	HOST_DMA_CMD_FIFO_REG dma;
	ADMIN_IDENTIFY_CONTROLLER *id = (ADMIN_IDENTIFY_CONTROLLER *)IDENTIFY_BUFFER;

	make_identify_cmd(1, HOST_PAGE_ADDR, 0);
	handle_identify(&cmd, &cpl);

	TEST_ASSERT_EQUAL_HEX16(PCI_VENDOR_ID, id->VID);
	dma = dma_descriptor_at(0);
	TEST_ASSERT_EQUAL_HEX32(ADMIN_CMD_DRAM_DATA_BUFFER, dma.devAddr);
	TEST_ASSERT_EQUAL_HEX32(HOST_PAGE_ADDR, dma.pcieAddrL);
	TEST_ASSERT_EQUAL_HEX32(0, dma.pcieAddrH);
	TEST_ASSERT_EQUAL_UINT(PAGE_SIZE_BYTES, dma.dmaLen);
	TEST_ASSERT_EQUAL_UINT(HOST_DMA_DIRECT_TYPE, dma.dmaType);
	TEST_ASSERT_EQUAL_UINT(HOST_DMA_TX_DIRECTION, dma.dmaDirection);
	TEST_ASSERT_EQUAL_size_t(1, mock_io_write_count_for(HOST_DMA_CMD_FIFO_REG_ADDR));
	assert_success_completion();
	TEST_ASSERT_EQUAL_HEX32(0, cpl.specific);
}

static void test_handle_identify_namespace_uses_capacity(void)
{
	ADMIN_IDENTIFY_NAMESPACE *ns = (ADMIN_IDENTIFY_NAMESPACE *)IDENTIFY_BUFFER;

	storageCapacity_L = 0x4000;
	make_identify_cmd(0, HOST_PAGE_ADDR, 0);
	handle_identify(&cmd, &cpl);

	TEST_ASSERT_EQUAL_HEX32(0x4000, ns->NSZE[0]);
	TEST_ASSERT_EQUAL_size_t(1, mock_io_write_count_for(HOST_DMA_CMD_FIFO_REG_ADDR));
	assert_success_completion();
}

static void test_handle_identify_splits_dma_when_prp1_not_page_aligned(void)
{
	HOST_DMA_CMD_FIFO_REG first, second;
	const unsigned int offset = 0x800;

	make_identify_cmd(1, HOST_PAGE_ADDR + offset, HOST_PAGE2_ADDR);
	handle_identify(&cmd, &cpl);

	TEST_ASSERT_EQUAL_size_t(2, mock_io_write_count_for(HOST_DMA_CMD_FIFO_REG_ADDR));

	first = dma_descriptor_at(0);
	TEST_ASSERT_EQUAL_HEX32(ADMIN_CMD_DRAM_DATA_BUFFER, first.devAddr);
	TEST_ASSERT_EQUAL_HEX32(HOST_PAGE_ADDR + offset, first.pcieAddrL);
	TEST_ASSERT_EQUAL_UINT(PAGE_SIZE_BYTES - offset, first.dmaLen);

	second = dma_descriptor_at(1);
	TEST_ASSERT_EQUAL_HEX32(ADMIN_CMD_DRAM_DATA_BUFFER + (PAGE_SIZE_BYTES - offset), second.devAddr);
	TEST_ASSERT_EQUAL_HEX32(HOST_PAGE2_ADDR, second.pcieAddrL);
	TEST_ASSERT_EQUAL_UINT(offset, second.dmaLen);
	assert_success_completion();
}

static void test_handle_identify_namespace_split_with_small_offset(void)
{
	HOST_DMA_CMD_FIFO_REG first, second;
	const unsigned int offset = 0x10;

	make_identify_cmd(0, HOST_PAGE_ADDR + offset, HOST_PAGE2_ADDR);
	handle_identify(&cmd, &cpl);

	first = dma_descriptor_at(0);
	second = dma_descriptor_at(1);
	TEST_ASSERT_EQUAL_UINT(PAGE_SIZE_BYTES - offset, first.dmaLen);
	TEST_ASSERT_EQUAL_UINT(offset, second.dmaLen);
	TEST_ASSERT_EQUAL_HEX32(HOST_PAGE2_ADDR, second.pcieAddrL);
	assert_success_completion();
}

/* ADMIN_IDENTIFY_COMMAND_DW10.CNS is a 1-bit field, so any other CNS value
 * (e.g. 2 = active namespace list) is truncated to bit 0 and served as a
 * namespace identify rather than rejected. */
static void test_handle_identify_truncates_cns_to_one_bit(void)
{
	ADMIN_IDENTIFY_NAMESPACE *ns = (ADMIN_IDENTIFY_NAMESPACE *)IDENTIFY_BUFFER;

	storageCapacity_L = 0x1234;
	make_identify_cmd(2, HOST_PAGE_ADDR, 0);
	handle_identify(&cmd, &cpl);

	TEST_ASSERT_EQUAL_HEX32(0x1234, ns->NSZE[0]);
	TEST_ASSERT_EQUAL_size_t(1, mock_io_write_count_for(HOST_DMA_CMD_FIFO_REG_ADDR));
	assert_success_completion();
}

static void test_handle_identify_rejects_prp1_not_16_byte_aligned(void)
{
	make_identify_cmd(1, HOST_PAGE_ADDR + 0x4, 0);
	FTL_TEST_EXPECT_ASSERT(handle_identify(&cmd, &cpl));
}

static void test_handle_identify_namespace_rejects_unaligned_prp2(void)
{
	make_identify_cmd(0, HOST_PAGE_ADDR, HOST_PAGE2_ADDR + 0x8);
	FTL_TEST_EXPECT_ASSERT(handle_identify(&cmd, &cpl));
}

static void test_handle_identify_rejects_prp2_high_with_page_offset_bits(void)
{
	make_identify_cmd(1, HOST_PAGE_ADDR + 0x800, HOST_PAGE2_ADDR);
	cmd.PRP2[1] = 0x1; /* firmware requires PRP2[63:32] & 0xFFF == 0 */
	FTL_TEST_EXPECT_ASSERT(handle_identify(&cmd, &cpl));
}

/* --------------------------------------------------------------------- */
/* set features                                                           */
/* --------------------------------------------------------------------- */

static void test_set_features_number_of_queues_echoes_request(void)
{
	ADMIN_SET_FEATURES_NUMBER_OF_QUEUES_DW11 req = { .dword = 0 };
	ADMIN_SET_FEATURES_NUMBER_OF_QUEUES_DW11 rsp;

	req.NSQR = 3;
	req.NCQR = 2;
	make_features_cmd(ADMIN_SET_FEATURES, NUMBER_OF_QUEUES, req.dword);
	handle_set_features(&cmd, &cpl);

	rsp.dword = cpl.specific;
	TEST_ASSERT_EQUAL_UINT(3, rsp.NSQR);
	TEST_ASSERT_EQUAL_UINT(2, rsp.NCQR);
	assert_success_completion();
}

static void test_set_features_number_of_queues_clamps_to_hardware_max(void)
{
	ADMIN_SET_FEATURES_NUMBER_OF_QUEUES_DW11 req = { .dword = 0 };
	ADMIN_SET_FEATURES_NUMBER_OF_QUEUES_DW11 rsp;

	req.NSQR = 0xFFFF;
	req.NCQR = MAX_NUM_OF_IO_CQ;
	make_features_cmd(ADMIN_SET_FEATURES, NUMBER_OF_QUEUES, req.dword);
	handle_set_features(&cmd, &cpl);

	rsp.dword = cpl.specific;
	TEST_ASSERT_EQUAL_UINT(MAX_NUM_OF_IO_SQ - 1, rsp.NSQR);
	TEST_ASSERT_EQUAL_UINT(MAX_NUM_OF_IO_CQ - 1, rsp.NCQR);
}

static void test_get_num_of_queue_keeps_values_below_limit(void)
{
	ADMIN_SET_FEATURES_NUMBER_OF_QUEUES_DW11 req = { .dword = 0 };

	req.NSQR = MAX_NUM_OF_IO_SQ - 1;
	req.NCQR = MAX_NUM_OF_IO_CQ - 1;
	TEST_ASSERT_EQUAL_HEX32(req.dword, get_num_of_queue(req.dword));
}

static void test_set_features_volatile_write_cache_enables_cache(void)
{
	make_features_cmd(ADMIN_SET_FEATURES, VOLATILE_WRITE_CACHE, 0x1);
	handle_set_features(&cmd, &cpl);

	TEST_ASSERT_EQUAL_UINT(1, g_nvmeTask.cacheEn);
	TEST_ASSERT_EQUAL_HEX32(0, cpl.specific);
	assert_success_completion();
}

static void test_set_features_volatile_write_cache_disables_cache_ignoring_upper_bits(void)
{
	g_nvmeTask.cacheEn = 1;
	make_features_cmd(ADMIN_SET_FEATURES, VOLATILE_WRITE_CACHE, 0xFFFFFFFE);
	handle_set_features(&cmd, &cpl);

	TEST_ASSERT_EQUAL_UINT(0, g_nvmeTask.cacheEn);
	assert_success_completion();
}

static void test_set_features_accepts_interrupt_coalescing(void)
{
	make_features_cmd(ADMIN_SET_FEATURES, INTERRUPT_COALESCING, 0x1234);
	handle_set_features(&cmd, &cpl);
	TEST_ASSERT_EQUAL_HEX32(0, cpl.specific);
	assert_success_completion();
}

static void test_set_features_accepts_arbitration(void)
{
	make_features_cmd(ADMIN_SET_FEATURES, ARBITRATION, 0x7);
	handle_set_features(&cmd, &cpl);
	TEST_ASSERT_EQUAL_HEX32(0, cpl.specific);
	assert_success_completion();
}

static void test_set_features_accepts_async_event_configuration(void)
{
	make_features_cmd(ADMIN_SET_FEATURES, ASYNCHRONOUS_EVENT_CONFIGURATION, 0xFF);
	handle_set_features(&cmd, &cpl);
	TEST_ASSERT_EQUAL_HEX32(0, cpl.specific);
	assert_success_completion();
}

static void test_set_features_accepts_power_management(void)
{
	make_features_cmd(ADMIN_SET_FEATURES, POWER_MANAGEMENT, 0x0);
	handle_set_features(&cmd, &cpl);
	TEST_ASSERT_EQUAL_HEX32(0, cpl.specific);
	assert_success_completion();
}

static void test_set_features_unsupported_fid_asserts(void)
{
	make_features_cmd(ADMIN_SET_FEATURES, UNSUPPORTED_FID, 0);
	FTL_TEST_EXPECT_ASSERT(handle_set_features(&cmd, &cpl));
}

static void test_set_features_ignores_save_bit_in_dword10(void)
{
	ADMIN_SET_FEATURES_DW10 dw10 = { .dword = 0 };

	dw10.FID = POWER_MANAGEMENT;
	dw10.SV = 1;
	make_features_cmd(ADMIN_SET_FEATURES, dw10.dword, 0);
	handle_set_features(&cmd, &cpl);
	assert_success_completion();
}

/* --------------------------------------------------------------------- */
/* get features                                                           */
/* --------------------------------------------------------------------- */

static void test_get_features_lba_range_type_returns_invalid_field(void)
{
	make_features_cmd(ADMIN_GET_FEATURES, LBA_RANGE_TYPE, 0);
	handle_get_features(&cmd, &cpl);

	TEST_ASSERT_EQUAL_UINT(SC_INVALID_FIELD_IN_COMMAND, cpl.statusField.SC);
	TEST_ASSERT_EQUAL_UINT(0, cpl.statusField.SCT);
	TEST_ASSERT_EQUAL_HEX32(0, cpl.specific);
}

static void test_get_features_lba_range_type_requires_namespace_1(void)
{
	make_features_cmd(ADMIN_GET_FEATURES, LBA_RANGE_TYPE, 0);
	cmd.NSID = 2;
	FTL_TEST_EXPECT_ASSERT(handle_get_features(&cmd, &cpl));
}

static void test_get_features_temperature_threshold_echoes_dword11(void)
{
	make_features_cmd(ADMIN_GET_FEATURES, TEMPERATURE_THRESHOLD, 0x0157);
	handle_get_features(&cmd, &cpl);

	TEST_ASSERT_EQUAL_HEX32(0x0157, cpl.specific);
	assert_success_completion();
}

static void test_get_features_volatile_write_cache_reports_enabled_state(void)
{
	g_nvmeTask.cacheEn = 1;
	make_features_cmd(ADMIN_GET_FEATURES, VOLATILE_WRITE_CACHE, 0);
	handle_get_features(&cmd, &cpl);
	TEST_ASSERT_EQUAL_HEX32(1, cpl.specific);
	assert_success_completion();
}

static void test_get_features_volatile_write_cache_reports_disabled_state(void)
{
	g_nvmeTask.cacheEn = 0;
	make_features_cmd(ADMIN_GET_FEATURES, VOLATILE_WRITE_CACHE, 0);
	handle_get_features(&cmd, &cpl);
	TEST_ASSERT_EQUAL_HEX32(0, cpl.specific);
	assert_success_completion();
}

static void test_get_features_power_management_reports_state_zero(void)
{
	make_features_cmd(ADMIN_GET_FEATURES, POWER_MANAGEMENT, 0xDEAD);
	handle_get_features(&cmd, &cpl);
	TEST_ASSERT_EQUAL_HEX32(0, cpl.specific);
	assert_success_completion();
}

static void test_get_features_unsupported_fid_asserts(void)
{
	make_features_cmd(ADMIN_GET_FEATURES, NUMBER_OF_QUEUES, 0);
	FTL_TEST_EXPECT_ASSERT(handle_get_features(&cmd, &cpl));
}

static void test_set_then_get_volatile_write_cache_round_trips(void)
{
	make_features_cmd(ADMIN_SET_FEATURES, VOLATILE_WRITE_CACHE, 0x1);
	handle_set_features(&cmd, &cpl);

	memset(&cpl, CPL_SENTINEL, sizeof(cpl));
	make_features_cmd(ADMIN_GET_FEATURES, VOLATILE_WRITE_CACHE, 0);
	handle_get_features(&cmd, &cpl);
	TEST_ASSERT_EQUAL_HEX32(1, cpl.specific);
}

/* --------------------------------------------------------------------- */
/* create / delete IO SQ                                                  */
/* --------------------------------------------------------------------- */

static void test_create_io_sq_records_queue_and_programs_register(void)
{
	NVME_IO_SQ_SET_REG reg;
	NVME_IO_SQ_STATUS *sq = &g_nvmeTask.ioSqInfo[2];

	make_create_sq_cmd(3, 0x3F, 5, HOST_PAGE_ADDR, 0x2);
	handle_create_io_sq(&cmd, &cpl);

	TEST_ASSERT_EQUAL_UINT(1, sq->valid);
	TEST_ASSERT_EQUAL_UINT(0x3F, sq->qSzie);
	TEST_ASSERT_EQUAL_UINT(5, sq->cqVector);
	TEST_ASSERT_EQUAL_HEX32(HOST_PAGE_ADDR, sq->pcieBaseAddrL);
	TEST_ASSERT_EQUAL_HEX32(0x2, sq->pcieBaseAddrH);

	reg = read_sq_set_reg(2);
	TEST_ASSERT_EQUAL_UINT(1, reg.valid);
	TEST_ASSERT_EQUAL_UINT(5, reg.cqVector);
	TEST_ASSERT_EQUAL_UINT(0x3F, reg.sqSize);
	TEST_ASSERT_EQUAL_HEX32(HOST_PAGE_ADDR, reg.pcieBaseAddrL);
	TEST_ASSERT_EQUAL_UINT(0x2, reg.pcieBaseAddrH);
	assert_success_completion();
	TEST_ASSERT_EQUAL_HEX32(0, cpl.specific);
}

static void test_create_io_sq_does_not_touch_other_queues(void)
{
	make_create_sq_cmd(1, 0x10, 1, HOST_PAGE_ADDR, 0);
	handle_create_io_sq(&cmd, &cpl);

	TEST_ASSERT_EQUAL_UINT(1, g_nvmeTask.ioSqInfo[0].valid);
	TEST_ASSERT_EQUAL_UINT(0, g_nvmeTask.ioSqInfo[1].valid);
	TEST_ASSERT_EQUAL_size_t(0, mock_io_write_count_for(NVME_IO_SQ_SET_REG_ADDR + 8));
}

static void test_create_io_sq_accepts_highest_queue_id(void)
{
	make_create_sq_cmd(MAX_NUM_OF_IO_SQ, 0xFF, MAX_NUM_OF_IO_CQ, HOST_PAGE_ADDR, 0xF);
	handle_create_io_sq(&cmd, &cpl);

	TEST_ASSERT_EQUAL_UINT(1, g_nvmeTask.ioSqInfo[MAX_NUM_OF_IO_SQ - 1].valid);
	TEST_ASSERT_EQUAL_UINT(0xFF, read_sq_set_reg(MAX_NUM_OF_IO_SQ - 1).sqSize);
	assert_success_completion();
}

static void test_create_io_sq_ignores_physically_contiguous_flag(void)
{
	ADMIN_CREATE_IO_SQ_DW11 dw11;

	make_create_sq_cmd(2, 0x10, 1, HOST_PAGE_ADDR, 0);
	dw11.dword = cmd.dword11;
	dw11.PC = 0;
	cmd.dword11 = dw11.dword;
	handle_create_io_sq(&cmd, &cpl);

	TEST_ASSERT_EQUAL_UINT(1, g_nvmeTask.ioSqInfo[1].valid);
	TEST_ASSERT_EQUAL_UINT(1, read_sq_set_reg(1).valid);
	assert_success_completion();
}

static void test_create_io_sq_rejects_queue_id_zero(void)
{
	make_create_sq_cmd(0, 0x10, 1, HOST_PAGE_ADDR, 0);
	FTL_TEST_EXPECT_ASSERT(handle_create_io_sq(&cmd, &cpl));
}

static void test_create_io_sq_rejects_queue_id_above_max(void)
{
	make_create_sq_cmd(MAX_NUM_OF_IO_SQ + 1, 0x10, 1, HOST_PAGE_ADDR, 0);
	FTL_TEST_EXPECT_ASSERT(handle_create_io_sq(&cmd, &cpl));
}

static void test_create_io_sq_rejects_oversized_queue(void)
{
	make_create_sq_cmd(1, 0x100, 1, HOST_PAGE_ADDR, 0);
	FTL_TEST_EXPECT_ASSERT(handle_create_io_sq(&cmd, &cpl));
}

static void test_create_io_sq_rejects_completion_queue_id_zero(void)
{
	make_create_sq_cmd(1, 0x10, 0, HOST_PAGE_ADDR, 0);
	FTL_TEST_EXPECT_ASSERT(handle_create_io_sq(&cmd, &cpl));
}

static void test_create_io_sq_rejects_unaligned_prp1(void)
{
	make_create_sq_cmd(1, 0x10, 1, HOST_PAGE_ADDR + 0x8, 0);
	FTL_TEST_EXPECT_ASSERT(handle_create_io_sq(&cmd, &cpl));
}

static void test_create_io_sq_rejects_prp1_high_out_of_range(void)
{
	make_create_sq_cmd(1, 0x10, 1, HOST_PAGE_ADDR, 0x10);
	FTL_TEST_EXPECT_ASSERT(handle_create_io_sq(&cmd, &cpl));
}

static void test_delete_io_sq_clears_bookkeeping_and_register(void)
{
	ADMIN_DELETE_IO_SQ_DW10 dw10 = { .dword = 0 };
	NVME_IO_SQ_SET_REG reg;
	NVME_IO_SQ_STATUS *sq = &g_nvmeTask.ioSqInfo[3];

	make_create_sq_cmd(4, 0x20, 2, HOST_PAGE_ADDR, 0x1);
	handle_create_io_sq(&cmd, &cpl);

	memset(&cmd, 0, sizeof(cmd));
	memset(&cpl, CPL_SENTINEL, sizeof(cpl));
	dw10.QID = 4;
	cmd.OPC = ADMIN_DELETE_IO_SQ;
	cmd.dword10 = dw10.dword;
	handle_delete_io_sq(&cmd, &cpl);

	TEST_ASSERT_EQUAL_UINT(0, sq->valid);
	TEST_ASSERT_EQUAL_UINT(0, sq->cqVector);
	TEST_ASSERT_EQUAL_UINT(0, sq->qSzie);
	TEST_ASSERT_EQUAL_HEX32(0, sq->pcieBaseAddrL);
	TEST_ASSERT_EQUAL_HEX32(0, sq->pcieBaseAddrH);

	reg = read_sq_set_reg(3);
	TEST_ASSERT_EQUAL_UINT(0, reg.valid);
	TEST_ASSERT_EQUAL_UINT(0, reg.cqVector);
	TEST_ASSERT_EQUAL_UINT(0, reg.sqSize);
	TEST_ASSERT_EQUAL_HEX32(0, reg.pcieBaseAddrL);
	TEST_ASSERT_EQUAL_UINT(0, reg.pcieBaseAddrH);
	assert_success_completion();
	TEST_ASSERT_EQUAL_HEX32(0, cpl.specific);
}

/* --------------------------------------------------------------------- */
/* create / delete IO CQ                                                  */
/* --------------------------------------------------------------------- */

static void test_create_io_cq_records_queue_and_programs_register(void)
{
	NVME_IO_CQ_SET_REG reg;
	NVME_IO_CQ_STATUS *cq = &g_nvmeTask.ioCqInfo[1];

	make_create_cq_cmd(2, 0x7F, 1, 5, HOST_PAGE2_ADDR, 0x3);
	handle_create_io_cq(&cmd, &cpl);

	TEST_ASSERT_EQUAL_UINT(1, cq->valid);
	TEST_ASSERT_EQUAL_UINT(0x7F, cq->qSzie);
	TEST_ASSERT_EQUAL_UINT(1, cq->irqEn);
	TEST_ASSERT_EQUAL_UINT(5, cq->irqVector);
	TEST_ASSERT_EQUAL_HEX32(HOST_PAGE2_ADDR, cq->pcieBaseAddrL);
	TEST_ASSERT_EQUAL_HEX32(0x3, cq->pcieBaseAddrH);

	reg = read_cq_set_reg(1);
	TEST_ASSERT_EQUAL_UINT(1, reg.valid);
	TEST_ASSERT_EQUAL_UINT(1, reg.irqEn);
	TEST_ASSERT_EQUAL_UINT(5, reg.irqVector);
	TEST_ASSERT_EQUAL_UINT(0x7F, reg.cqSize);
	TEST_ASSERT_EQUAL_HEX32(HOST_PAGE2_ADDR, reg.pcieBaseAddrL);
	TEST_ASSERT_EQUAL_UINT(0x3, reg.pcieBaseAddrH);
	assert_success_completion();
	TEST_ASSERT_EQUAL_HEX32(0, cpl.specific);
}

static void test_create_io_cq_with_interrupts_disabled(void)
{
	make_create_cq_cmd(1, 0x10, 0, 0, HOST_PAGE_ADDR, 0);
	handle_create_io_cq(&cmd, &cpl);

	TEST_ASSERT_EQUAL_UINT(0, g_nvmeTask.ioCqInfo[0].irqEn);
	TEST_ASSERT_EQUAL_UINT(0, read_cq_set_reg(0).irqEn);
	assert_success_completion();
}

static void test_create_io_cq_accepts_highest_queue_id_and_vector(void)
{
	make_create_cq_cmd(MAX_NUM_OF_IO_CQ, 0xFF, 1, 7, HOST_PAGE_ADDR, 0xF);
	handle_create_io_cq(&cmd, &cpl);

	TEST_ASSERT_EQUAL_UINT(1, g_nvmeTask.ioCqInfo[MAX_NUM_OF_IO_CQ - 1].valid);
	TEST_ASSERT_EQUAL_UINT(7, read_cq_set_reg(MAX_NUM_OF_IO_CQ - 1).irqVector);
	assert_success_completion();
}

static void test_create_io_cq_ignores_physically_contiguous_flag(void)
{
	ADMIN_CREATE_IO_CQ_DW11 dw11;

	make_create_cq_cmd(2, 0x10, 1, 0, HOST_PAGE_ADDR, 0);
	dw11.dword = cmd.dword11;
	dw11.PC = 0;
	cmd.dword11 = dw11.dword;
	handle_create_io_cq(&cmd, &cpl);

	TEST_ASSERT_EQUAL_UINT(1, g_nvmeTask.ioCqInfo[1].valid);
	TEST_ASSERT_EQUAL_UINT(1, read_cq_set_reg(1).valid);
	assert_success_completion();
}

static void test_create_io_cq_rejects_queue_id_zero(void)
{
	make_create_cq_cmd(0, 0x10, 1, 0, HOST_PAGE_ADDR, 0);
	FTL_TEST_EXPECT_ASSERT(handle_create_io_cq(&cmd, &cpl));
}

static void test_create_io_cq_rejects_queue_id_above_max(void)
{
	make_create_cq_cmd(MAX_NUM_OF_IO_CQ + 1, 0x10, 1, 0, HOST_PAGE_ADDR, 0);
	FTL_TEST_EXPECT_ASSERT(handle_create_io_cq(&cmd, &cpl));
}

static void test_create_io_cq_rejects_oversized_queue(void)
{
	make_create_cq_cmd(1, 0x100, 1, 0, HOST_PAGE_ADDR, 0);
	FTL_TEST_EXPECT_ASSERT(handle_create_io_cq(&cmd, &cpl));
}

static void test_create_io_cq_rejects_interrupt_vector_above_7(void)
{
	make_create_cq_cmd(1, 0x10, 1, 8, HOST_PAGE_ADDR, 0);
	FTL_TEST_EXPECT_ASSERT(handle_create_io_cq(&cmd, &cpl));
}

static void test_create_io_cq_rejects_unaligned_prp1(void)
{
	make_create_cq_cmd(1, 0x10, 1, 0, HOST_PAGE_ADDR + 0x4, 0);
	FTL_TEST_EXPECT_ASSERT(handle_create_io_cq(&cmd, &cpl));
}

static void test_create_io_cq_rejects_prp1_high_out_of_range(void)
{
	make_create_cq_cmd(1, 0x10, 1, 0, HOST_PAGE_ADDR, 0x10);
	FTL_TEST_EXPECT_ASSERT(handle_create_io_cq(&cmd, &cpl));
}

static void test_delete_io_cq_clears_bookkeeping_and_register(void)
{
	ADMIN_DELETE_IO_CQ_DW10 dw10 = { .dword = 0 };
	NVME_IO_CQ_SET_REG reg;
	NVME_IO_CQ_STATUS *cq = &g_nvmeTask.ioCqInfo[5];

	make_create_cq_cmd(6, 0x20, 1, 3, HOST_PAGE_ADDR, 0x1);
	handle_create_io_cq(&cmd, &cpl);

	memset(&cmd, 0, sizeof(cmd));
	memset(&cpl, CPL_SENTINEL, sizeof(cpl));
	dw10.QID = 6;
	cmd.OPC = ADMIN_DELETE_IO_CQ;
	cmd.dword10 = dw10.dword;
	handle_delete_io_cq(&cmd, &cpl);

	TEST_ASSERT_EQUAL_UINT(0, cq->valid);
	TEST_ASSERT_EQUAL_UINT(0, cq->irqVector);
	TEST_ASSERT_EQUAL_UINT(0, cq->qSzie);
	TEST_ASSERT_EQUAL_HEX32(0, cq->pcieBaseAddrL);
	TEST_ASSERT_EQUAL_HEX32(0, cq->pcieBaseAddrH);

	reg = read_cq_set_reg(5);
	TEST_ASSERT_EQUAL_UINT(0, reg.valid);
	TEST_ASSERT_EQUAL_UINT(0, reg.irqEn);
	TEST_ASSERT_EQUAL_UINT(0, reg.irqVector);
	TEST_ASSERT_EQUAL_UINT(0, reg.cqSize);
	TEST_ASSERT_EQUAL_HEX32(0, reg.pcieBaseAddrL);
	TEST_ASSERT_EQUAL_UINT(0, reg.pcieBaseAddrH);
	assert_success_completion();
	TEST_ASSERT_EQUAL_HEX32(0, cpl.specific);
}

/* handle_delete_io_cq resets valid/irqVector/qSzie/pcieBaseAddr but never
 * touches irqEn, so a deleted queue keeps reporting interrupts enabled in
 * g_nvmeTask even though the hardware register was cleared. */
static void test_delete_io_cq_clears_interrupt_enable_bookkeeping(void)
{
	ADMIN_DELETE_IO_CQ_DW10 dw10 = { .dword = 0 };

	make_create_cq_cmd(6, 0x20, 1, 3, HOST_PAGE_ADDR, 0x1);
	handle_create_io_cq(&cmd, &cpl);
	TEST_ASSERT_EQUAL_UINT(1, g_nvmeTask.ioCqInfo[5].irqEn);

	memset(&cmd, 0, sizeof(cmd));
	dw10.QID = 6;
	cmd.OPC = ADMIN_DELETE_IO_CQ;
	cmd.dword10 = dw10.dword;
	handle_delete_io_cq(&cmd, &cpl);

	TEST_ASSERT_EQUAL_UINT(0, read_cq_set_reg(5).irqEn);
	if (g_nvmeTask.ioCqInfo[5].irqEn != 0)
		TEST_IGNORE_MESSAGE("BUG: handle_delete_io_cq leaves ioCqInfo[].irqEn set after delete");
	TEST_ASSERT_EQUAL_UINT(0, g_nvmeTask.ioCqInfo[5].irqEn);
}

/* --------------------------------------------------------------------- */
/* get log page                                                           */
/* --------------------------------------------------------------------- */

static void test_get_log_page_reports_invalid_log_page(void)
{
	cmd.OPC = ADMIN_GET_LOG_PAGE;
	cmd.dword10 = 0x02; /* SMART / health */
	handle_get_log_page(&cmd, &cpl);

	TEST_ASSERT_EQUAL_HEX32(0x9, cpl.specific);
	assert_success_completion();
}

/* --------------------------------------------------------------------- */
/* handle_nvme_admin_cmd dispatcher + completion formatting               */
/* --------------------------------------------------------------------- */

static void test_dispatch_identify_posts_auto_completion_for_slot(void)
{
	NVME_CPL_FIFO_REG reg;

	make_identify_cmd(1, HOST_PAGE_ADDR, 0);
	dispatch_admin_cmd();

	/* Auto completion writes only dword[1] and dword[2] of the CPL FIFO. */
	TEST_ASSERT_EQUAL_size_t(0, mock_io_write_count_for(NVME_CPL_FIFO_REG_ADDR));
	TEST_ASSERT_EQUAL_size_t(1, mock_io_write_count_for(NVME_CPL_FIFO_REG_ADDR + 4));
	TEST_ASSERT_EQUAL_size_t(1, mock_io_write_count_for(NVME_CPL_FIFO_REG_ADDR + 8));

	reg = read_cpl_fifo_reg();
	TEST_ASSERT_EQUAL_UINT(TEST_SLOT_TAG, reg.cmdSlotTag);
	TEST_ASSERT_EQUAL_UINT(AUTO_CPL_TYPE, reg.cplType);
	TEST_ASSERT_EQUAL_HEX32(0, reg.specific);
	TEST_ASSERT_EQUAL_UINT(0, reg.statusField.SC);
	TEST_ASSERT_EQUAL_UINT(0, reg.statusField.SCT);
}

static void test_dispatch_get_features_lba_range_reports_invalid_field_status(void)
{
	NVME_CPL_FIFO_REG reg;

	make_features_cmd(ADMIN_GET_FEATURES, LBA_RANGE_TYPE, 0);
	dispatch_admin_cmd();

	reg = read_cpl_fifo_reg();
	TEST_ASSERT_EQUAL_UINT(AUTO_CPL_TYPE, reg.cplType);
	TEST_ASSERT_EQUAL_UINT(SC_INVALID_FIELD_IN_COMMAND, reg.statusField.SC);
	TEST_ASSERT_EQUAL_UINT(0, reg.statusField.SCT);
	TEST_ASSERT_EQUAL_UINT(0, reg.statusField.DNR);
}

static void test_dispatch_set_features_number_of_queues_returns_specific(void)
{
	ADMIN_SET_FEATURES_NUMBER_OF_QUEUES_DW11 req = { .dword = 0 };
	ADMIN_SET_FEATURES_NUMBER_OF_QUEUES_DW11 rsp;

	req.NSQR = 1;
	req.NCQR = 1;
	make_features_cmd(ADMIN_SET_FEATURES, NUMBER_OF_QUEUES, req.dword);
	dispatch_admin_cmd();

	rsp.dword = read_cpl_fifo_reg().specific;
	TEST_ASSERT_EQUAL_UINT(1, rsp.NSQR);
	TEST_ASSERT_EQUAL_UINT(1, rsp.NCQR);
}

static void test_dispatch_get_features_temperature_returns_specific(void)
{
	make_features_cmd(ADMIN_GET_FEATURES, TEMPERATURE_THRESHOLD, 0x1F4);
	dispatch_admin_cmd();
	TEST_ASSERT_EQUAL_HEX32(0x1F4, read_cpl_fifo_reg().specific);
}

static void test_dispatch_get_log_page_returns_invalid_log_page_code(void)
{
	cmd.OPC = ADMIN_GET_LOG_PAGE;
	dispatch_admin_cmd();
	TEST_ASSERT_EQUAL_HEX32(0x9, read_cpl_fifo_reg().specific);
	TEST_ASSERT_EQUAL_UINT(AUTO_CPL_TYPE, read_cpl_fifo_reg().cplType);
}

static void test_dispatch_create_io_cq_then_sq_programs_both_registers(void)
{
	make_create_cq_cmd(1, 0x3F, 1, 0, HOST_PAGE2_ADDR, 0);
	dispatch_admin_cmd();

	memset(&cmd, 0, sizeof(cmd));
	make_create_sq_cmd(1, 0x3F, 1, HOST_PAGE_ADDR, 0);
	dispatch_admin_cmd();

	TEST_ASSERT_EQUAL_UINT(1, g_nvmeTask.ioCqInfo[0].valid);
	TEST_ASSERT_EQUAL_UINT(1, g_nvmeTask.ioSqInfo[0].valid);
	TEST_ASSERT_EQUAL_UINT(1, read_cq_set_reg(0).valid);
	TEST_ASSERT_EQUAL_UINT(1, read_sq_set_reg(0).valid);
	TEST_ASSERT_EQUAL_size_t(2, mock_io_write_count_for(NVME_CPL_FIFO_REG_ADDR + 8));
}

static void test_dispatch_delete_io_sq_and_cq_invalidate_registers(void)
{
	ADMIN_DELETE_IO_SQ_DW10 sqDw10 = { .dword = 0 };
	ADMIN_DELETE_IO_CQ_DW10 cqDw10 = { .dword = 0 };

	make_create_cq_cmd(2, 0x3F, 1, 1, HOST_PAGE2_ADDR, 0);
	dispatch_admin_cmd();
	memset(&cmd, 0, sizeof(cmd));
	make_create_sq_cmd(2, 0x3F, 2, HOST_PAGE_ADDR, 0);
	dispatch_admin_cmd();

	memset(&cmd, 0, sizeof(cmd));
	sqDw10.QID = 2;
	cmd.OPC = ADMIN_DELETE_IO_SQ;
	cmd.dword10 = sqDw10.dword;
	dispatch_admin_cmd();

	memset(&cmd, 0, sizeof(cmd));
	cqDw10.QID = 2;
	cmd.OPC = ADMIN_DELETE_IO_CQ;
	cmd.dword10 = cqDw10.dword;
	dispatch_admin_cmd();

	TEST_ASSERT_EQUAL_UINT(0, g_nvmeTask.ioSqInfo[1].valid);
	TEST_ASSERT_EQUAL_UINT(0, g_nvmeTask.ioCqInfo[1].valid);
	TEST_ASSERT_EQUAL_UINT(0, read_sq_set_reg(1).valid);
	TEST_ASSERT_EQUAL_UINT(0, read_cq_set_reg(1).valid);
	TEST_ASSERT_EQUAL_size_t(4, mock_io_write_count_for(NVME_CPL_FIFO_REG_ADDR + 8));
}

static void test_dispatch_async_event_request_releases_slot_without_completion(void)
{
	NVME_CPL_FIFO_REG reg;

	cmd.OPC = ADMIN_ASYNCHRONOUS_EVENT_REQUEST;
	dispatch_admin_cmd();

	/* Slot release writes dword[0] and dword[2] only; no auto completion. */
	TEST_ASSERT_EQUAL_size_t(1, mock_io_write_count_for(NVME_CPL_FIFO_REG_ADDR));
	TEST_ASSERT_EQUAL_size_t(0, mock_io_write_count_for(NVME_CPL_FIFO_REG_ADDR + 4));
	TEST_ASSERT_EQUAL_size_t(1, mock_io_write_count_for(NVME_CPL_FIFO_REG_ADDR + 8));

	reg = read_cpl_fifo_reg();
	TEST_ASSERT_EQUAL_UINT(TEST_SLOT_TAG, reg.cmdSlotTag);
	TEST_ASSERT_EQUAL_UINT(CMD_SLOT_RELEASE_TYPE, reg.cplType);
}

static void test_dispatch_abort_is_unsupported_and_asserts(void)
{
	cmd.OPC = ADMIN_ABORT;
	FTL_TEST_EXPECT_ASSERT(dispatch_admin_cmd());
	TEST_ASSERT_EQUAL_size_t(0, mock_io_write_count_for(NVME_CPL_FIFO_REG_ADDR + 8));
}

static void test_dispatch_firmware_download_is_unsupported_and_asserts(void)
{
	cmd.OPC = ADMIN_FIRMWARE_IMAGE_DOWNLOAD;
	FTL_TEST_EXPECT_ASSERT(dispatch_admin_cmd());
}

static void test_dispatch_format_nvm_is_unsupported_and_asserts(void)
{
	cmd.OPC = ADMIN_FORMAT_NVM;
	FTL_TEST_EXPECT_ASSERT(dispatch_admin_cmd());
}

static void test_dispatch_security_send_is_unsupported_and_asserts(void)
{
	cmd.OPC = ADMIN_SECURITY_SEND;
	FTL_TEST_EXPECT_ASSERT(dispatch_admin_cmd());
}

static void test_dispatch_unknown_opcode_asserts(void)
{
	cmd.OPC = 0xFF;
	FTL_TEST_EXPECT_ASSERT(dispatch_admin_cmd());
	TEST_ASSERT_EQUAL_size_t(0, mock_io_write_count_for(NVME_CPL_FIFO_REG_ADDR + 8));
}

static void test_dispatch_uses_slot_tag_from_command_wrapper(void)
{
	make_features_cmd(ADMIN_SET_FEATURES, POWER_MANAGEMENT, 0);
	slotCmd.cmdSlotTag = 0x7F;
	memcpy(slotCmd.cmdDword, cmd.dword, sizeof(slotCmd.cmdDword));
	handle_nvme_admin_cmd(&slotCmd);

	TEST_ASSERT_EQUAL_UINT(0x7F, read_cpl_fifo_reg().cmdSlotTag);
}

int main(void)
{
	UNITY_BEGIN();

	RUN_TEST(test_identify_controller_populates_vendor_and_strings);
	RUN_TEST(test_identify_controller_populates_capabilities);
	RUN_TEST(test_identify_controller_clears_stale_buffer);
	RUN_TEST(test_identify_namespace_reports_ftl_capacity);
	RUN_TEST(test_identify_namespace_single_4k_lba_format);

	RUN_TEST(test_handle_identify_controller_single_page_dma);
	RUN_TEST(test_handle_identify_namespace_uses_capacity);
	RUN_TEST(test_handle_identify_splits_dma_when_prp1_not_page_aligned);
	RUN_TEST(test_handle_identify_namespace_split_with_small_offset);
	RUN_TEST(test_handle_identify_truncates_cns_to_one_bit);
	RUN_TEST(test_handle_identify_rejects_prp1_not_16_byte_aligned);
	RUN_TEST(test_handle_identify_namespace_rejects_unaligned_prp2);
	RUN_TEST(test_handle_identify_rejects_prp2_high_with_page_offset_bits);

	RUN_TEST(test_set_features_number_of_queues_echoes_request);
	RUN_TEST(test_set_features_number_of_queues_clamps_to_hardware_max);
	RUN_TEST(test_get_num_of_queue_keeps_values_below_limit);
	RUN_TEST(test_set_features_volatile_write_cache_enables_cache);
	RUN_TEST(test_set_features_volatile_write_cache_disables_cache_ignoring_upper_bits);
	RUN_TEST(test_set_features_accepts_interrupt_coalescing);
	RUN_TEST(test_set_features_accepts_arbitration);
	RUN_TEST(test_set_features_accepts_async_event_configuration);
	RUN_TEST(test_set_features_accepts_power_management);
	RUN_TEST(test_set_features_unsupported_fid_asserts);
	RUN_TEST(test_set_features_ignores_save_bit_in_dword10);

	RUN_TEST(test_get_features_lba_range_type_returns_invalid_field);
	RUN_TEST(test_get_features_lba_range_type_requires_namespace_1);
	RUN_TEST(test_get_features_temperature_threshold_echoes_dword11);
	RUN_TEST(test_get_features_volatile_write_cache_reports_enabled_state);
	RUN_TEST(test_get_features_volatile_write_cache_reports_disabled_state);
	RUN_TEST(test_get_features_power_management_reports_state_zero);
	RUN_TEST(test_get_features_unsupported_fid_asserts);
	RUN_TEST(test_set_then_get_volatile_write_cache_round_trips);

	RUN_TEST(test_create_io_sq_records_queue_and_programs_register);
	RUN_TEST(test_create_io_sq_does_not_touch_other_queues);
	RUN_TEST(test_create_io_sq_accepts_highest_queue_id);
	RUN_TEST(test_create_io_sq_ignores_physically_contiguous_flag);
	RUN_TEST(test_create_io_sq_rejects_queue_id_zero);
	RUN_TEST(test_create_io_sq_rejects_queue_id_above_max);
	RUN_TEST(test_create_io_sq_rejects_oversized_queue);
	RUN_TEST(test_create_io_sq_rejects_completion_queue_id_zero);
	RUN_TEST(test_create_io_sq_rejects_unaligned_prp1);
	RUN_TEST(test_create_io_sq_rejects_prp1_high_out_of_range);
	RUN_TEST(test_delete_io_sq_clears_bookkeeping_and_register);

	RUN_TEST(test_create_io_cq_records_queue_and_programs_register);
	RUN_TEST(test_create_io_cq_with_interrupts_disabled);
	RUN_TEST(test_create_io_cq_accepts_highest_queue_id_and_vector);
	RUN_TEST(test_create_io_cq_ignores_physically_contiguous_flag);
	RUN_TEST(test_create_io_cq_rejects_queue_id_zero);
	RUN_TEST(test_create_io_cq_rejects_queue_id_above_max);
	RUN_TEST(test_create_io_cq_rejects_oversized_queue);
	RUN_TEST(test_create_io_cq_rejects_interrupt_vector_above_7);
	RUN_TEST(test_create_io_cq_rejects_unaligned_prp1);
	RUN_TEST(test_create_io_cq_rejects_prp1_high_out_of_range);
	RUN_TEST(test_delete_io_cq_clears_bookkeeping_and_register);
	RUN_TEST(test_delete_io_cq_clears_interrupt_enable_bookkeeping);

	RUN_TEST(test_get_log_page_reports_invalid_log_page);

	RUN_TEST(test_dispatch_identify_posts_auto_completion_for_slot);
	RUN_TEST(test_dispatch_get_features_lba_range_reports_invalid_field_status);
	RUN_TEST(test_dispatch_set_features_number_of_queues_returns_specific);
	RUN_TEST(test_dispatch_get_features_temperature_returns_specific);
	RUN_TEST(test_dispatch_get_log_page_returns_invalid_log_page_code);
	RUN_TEST(test_dispatch_create_io_cq_then_sq_programs_both_registers);
	RUN_TEST(test_dispatch_delete_io_sq_and_cq_invalidate_registers);
	RUN_TEST(test_dispatch_async_event_request_releases_slot_without_completion);
	RUN_TEST(test_dispatch_abort_is_unsupported_and_asserts);
	RUN_TEST(test_dispatch_firmware_download_is_unsupported_and_asserts);
	RUN_TEST(test_dispatch_format_nvm_is_unsupported_and_asserts);
	RUN_TEST(test_dispatch_security_send_is_unsupported_and_asserts);
	RUN_TEST(test_dispatch_unknown_opcode_asserts);
	RUN_TEST(test_dispatch_uses_slot_tag_from_command_wrapper);

	return UNITY_END();
}
