/* Unit tests for nvme/nvme_main.c.
 *
 * nvme_main() boots the FTL and then spins forever in the NVMe controller
 * state machine. Each test pre-sets g_nvmeTask.status, models the host
 * controller registers through mock_io, runs nvme_main() under
 * FTL_TEST_RUN_UNTIL_ESCAPE and unwinds from a register read handler or a
 * printf hook once the transition under test has been observed. */
#include <string.h>
#include "unity.h"
#include "ftl_test_env.h"
#include "ftl_config.h"
#include "request_allocation.h"
#include "request_format.h"
#include "address_translation.h"
#include "mock_nsc.h"
#include "nvme/nvme.h"
#include "nvme/host_lld.h"
#include "nvme/nvme_main.h"

extern volatile NVME_CONTEXT g_nvmeTask;

#define IO_QUEUE_COUNT          8u
#define IO_QUEUE_REG_WRITES     (IO_QUEUE_COUNT * 2u)   /* two dwords per queue */
#define NVME_CMD_DWORDS         16u
#define CMD_SLOT_BYTES          64u
#define TEST_SLOT_TAG           5u
#define TEST_SEQ_NUM            0x2Au
#define CPL_FIFO_DWORD1_ADDR    (NVME_CPL_FIFO_REG_ADDR + 4)
#define CPL_FIFO_DWORD2_ADDR    (NVME_CPL_FIFO_REG_ADDR + 8)

void setUp(void)
{
	ftl_test_env_reset();
	memset((void *)&g_nvmeTask, 0, sizeof(g_nvmeTask));
}
void tearDown(void) {}

/* --- escape helpers ------------------------------------------------------ */

typedef struct
{
	unsigned int reads;
	unsigned int escapeOnRead;
	uint32_t valueBeforeEscape;
} counted_read_t;

static uint32_t counted_read_handler(uintptr_t addr, uint32_t stored, void *ctx)
{
	counted_read_t *c = ctx;
	(void)addr;
	c->reads++;
	if (c->reads >= c->escapeOnRead)
		ftl_test_env_escape();
	return c->valueBeforeEscape ? c->valueBeforeEscape : stored;
}

static void escape_on_nth_read(uintptr_t addr, counted_read_t *c, unsigned int n)
{
	memset(c, 0, sizeof(*c));
	c->escapeOnRead = n;
	mock_io_set_read_handler(addr, counted_read_handler, c);
}

static void escape_on_printf(const char *fmt, void *ctx)
{
	if (strcmp(fmt, (const char *)ctx) == 0)
		ftl_test_env_escape();
}

static void escape_when_firmware_prints(const char *message)
{
	ftl_test_set_printf_hook(escape_on_printf, (void *)message);
}

/* Books a grown-bad update on die 0 once InitFTL() has finished (it clears
 * the flag during boot), then escapes on the shutdown banner. */
static void book_grown_bad_then_escape_on_shutdown(const char *fmt, void *ctx)
{
	(void)ctx;
	if (strcmp(fmt, "[ ftl configuration complete. ]\r\n") == 0)
		bbtInfoMapPtr->bbtInfo[0].grownBadUpdate = BBT_INFO_GROWN_BAD_UPDATE_BOOKED;
	else if (strcmp(fmt, "\r\nNVMe shutdown!!!\r\n") == 0)
		ftl_test_env_escape();
}

static void set_status_reg(unsigned int ccEn, unsigned int ccShn)
{
	NVME_STATUS_REG reg;
	reg.dword = 0;
	reg.ccEn = ccEn;
	reg.ccShn = ccShn;
	mock_io_set_reg(NVME_STATUS_REG_ADDR, reg.dword);
}

static NVME_STATUS_REG last_status_write(void)
{
	NVME_STATUS_REG reg;
	int found;
	reg.dword = mock_io_last_write(NVME_STATUS_REG_ADDR, &found);
	TEST_ASSERT_TRUE_MESSAGE(found, "NVMe status register was never written");
	return reg;
}

static NVME_ADMIN_QUEUE_SET_REG last_admin_queue_write(void)
{
	NVME_ADMIN_QUEUE_SET_REG reg;
	int found;
	reg.dword = mock_io_last_write(NVME_ADMIN_QUEUE_SET_REG_ADDR, &found);
	TEST_ASSERT_TRUE_MESSAGE(found, "admin queue register was never written");
	return reg;
}

static size_t io_queue_reg_writes(uintptr_t base)
{
	size_t total = 0;
	unsigned int q;
	for (q = 0; q < IO_QUEUE_COUNT; q++)
	{
		total += mock_io_write_count_for(base + q * 8);
		total += mock_io_write_count_for(base + q * 8 + 4);
	}
	return total;
}

/* Fill the command SRAM slot and program the CMD FIFO register so the first
 * get_nvme_cmd() returns a command and later polls return "no command". */
typedef struct
{
	counted_read_t counter;
	uint32_t fifoWord;
} cmd_fifo_t;

static uint32_t cmd_fifo_handler(uintptr_t addr, uint32_t stored, void *ctx)
{
	cmd_fifo_t *f = ctx;
	(void)addr; (void)stored;
	f->counter.reads++;
	if (f->counter.reads >= f->counter.escapeOnRead)
		ftl_test_env_escape();
	return f->counter.reads == 1 ? f->fifoWord : 0;
}

static void queue_host_command(cmd_fifo_t *f, unsigned int qID, const unsigned int *dwords,
                               unsigned int escapeOnRead)
{
	NVME_CMD_FIFO_REG reg;
	unsigned int idx;
	uintptr_t sram = NVME_CMD_SRAM_ADDR + TEST_SLOT_TAG * CMD_SLOT_BYTES;

	for (idx = 0; idx < NVME_CMD_DWORDS; idx++)
		mock_io_set_reg(sram + idx * 4, dwords[idx]);

	reg.dword = 0;
	reg.cmdValid = 1;
	reg.qID = qID;
	reg.cmdSlotTag = TEST_SLOT_TAG;
	reg.cmdSeqNum = TEST_SEQ_NUM;

	memset(f, 0, sizeof(*f));
	f->fifoWord = reg.dword;
	f->counter.escapeOnRead = escapeOnRead;
	mock_io_set_read_handler(NVME_CMD_FIFO_REG_ADDR, cmd_fifo_handler, f);
}

/* --- boot ---------------------------------------------------------------- */

static void test_main_boots_ftl_before_entering_state_machine(void)
{
	counted_read_t c;

	g_nvmeTask.status = NVME_TASK_WAIT_CC_EN;
	set_status_reg(0, 0);
	escape_on_nth_read(NVME_STATUS_REG_ADDR, &c, 1);

	FTL_TEST_RUN_UNTIL_ESCAPE(nvme_main());

	TEST_ASSERT_NOT_EQUAL_UINT(0, storageCapacity_L);
	TEST_ASSERT_NOT_NULL(reqPoolPtr);
	TEST_ASSERT_EQUAL_UINT(REQ_SLOT_TAG_NONE, sliceReqQ.headReq);
	TEST_ASSERT_GREATER_THAN_size_t(0, mock_nsc_call_count());
}

/* --- NVME_TASK_WAIT_CC_EN ------------------------------------------------- */

static void test_wait_cc_en_stays_while_controller_disabled(void)
{
	counted_read_t c;

	g_nvmeTask.status = NVME_TASK_WAIT_CC_EN;
	set_status_reg(0, 0);
	escape_on_nth_read(NVME_STATUS_REG_ADDR, &c, 3);

	FTL_TEST_RUN_UNTIL_ESCAPE(nvme_main());

	TEST_ASSERT_EQUAL_UINT(NVME_TASK_WAIT_CC_EN, g_nvmeTask.status);
	TEST_ASSERT_EQUAL_size_t(0, mock_io_write_count_for(NVME_STATUS_REG_ADDR));
	TEST_ASSERT_EQUAL_size_t(0, mock_io_write_count_for(NVME_ADMIN_QUEUE_SET_REG_ADDR));
}

static void test_wait_cc_en_enables_admin_queue_and_ready_when_cc_en_set(void)
{
	NVME_ADMIN_QUEUE_SET_REG adminReg;

	g_nvmeTask.status = NVME_TASK_WAIT_CC_EN;
	set_status_reg(1, 0);
	escape_when_firmware_prints("\r\nNVMe ready!!!\r\n");

	FTL_TEST_RUN_UNTIL_ESCAPE(nvme_main());

	TEST_ASSERT_EQUAL_UINT(NVME_TASK_RUNNING, g_nvmeTask.status);
	adminReg = last_admin_queue_write();
	TEST_ASSERT_EQUAL_UINT(1, adminReg.sqValid);
	TEST_ASSERT_EQUAL_UINT(1, adminReg.cqValid);
	TEST_ASSERT_EQUAL_UINT(1, adminReg.cqIrqEn);
	TEST_ASSERT_EQUAL_UINT(1, last_status_write().cstsRdy);
	TEST_ASSERT_EQUAL_UINT(1, last_status_write().ccEn);
}

/* --- NVME_TASK_RUNNING ---------------------------------------------------- */

static void test_running_polls_command_fifo_when_no_command_pending(void)
{
	counted_read_t c;

	g_nvmeTask.status = NVME_TASK_RUNNING;
	escape_on_nth_read(NVME_CMD_FIFO_REG_ADDR, &c, 3);

	FTL_TEST_RUN_UNTIL_ESCAPE(nvme_main());

	TEST_ASSERT_EQUAL_UINT(NVME_TASK_RUNNING, g_nvmeTask.status);
	TEST_ASSERT_EQUAL_UINT(3, c.reads);
	TEST_ASSERT_EQUAL_UINT(REQ_SLOT_TAG_NONE, sliceReqQ.headReq);
	TEST_ASSERT_EQUAL_size_t(0, mock_io_write_count_for(NVME_CPL_FIFO_REG_ADDR));
	TEST_ASSERT_EQUAL_size_t(0, mock_io_write_count_for(CPL_FIFO_DWORD2_ADDR));
}

static void test_running_dispatches_io_read_and_schedules_low_level(void)
{
	cmd_fifo_t fifo;
	unsigned int dwords[NVME_CMD_DWORDS];
	NVME_IO_COMMAND *io = (NVME_IO_COMMAND *)dwords;

	memset(dwords, 0, sizeof(dwords));
	io->OPC = IO_NVM_READ;
	io->NSID = 1;
	io->PRP1[0] = 0x20000000;
	io->dword10 = 0;
	io->dword12 = NVME_BLOCKS_PER_SLICE - 1;

	g_nvmeTask.status = NVME_TASK_RUNNING;
	/* read 1: command; read 2: idle poll (scheduler tail runs); read 3: escape */
	queue_host_command(&fifo, 1, dwords, 3);

	FTL_TEST_RUN_UNTIL_ESCAPE(nvme_main());

	TEST_ASSERT_EQUAL_UINT(NVME_TASK_RUNNING, g_nvmeTask.status);
	/* Slice queue drained by ReqTransSliceToLowLevel(). */
	TEST_ASSERT_EQUAL_UINT(REQ_SLOT_TAG_NONE, sliceReqQ.headReq);
	/* Read of an unwritten slice: the firmware completes it from the host
	 * DMA path and releases the command slot. */
	TEST_ASSERT_GREATER_THAN_size_t(0, mock_io_write_count_for(HOST_DMA_CMD_FIFO_REG_ADDR));
	TEST_ASSERT_GREATER_THAN_size_t(0, mock_io_read_count());
}

static void test_running_io_write_allocates_buffer_and_requests_host_dma(void)
{
	cmd_fifo_t fifo;
	unsigned int dwords[NVME_CMD_DWORDS];
	NVME_IO_COMMAND *io = (NVME_IO_COMMAND *)dwords;
	HOST_DMA_CMD_FIFO_REG dma;
	int found;

	memset(dwords, 0, sizeof(dwords));
	io->OPC = IO_NVM_WRITE;
	io->NSID = 1;
	io->PRP1[0] = 0x20000000;
	io->dword10 = NVME_BLOCKS_PER_SLICE * 4;
	io->dword12 = NVME_BLOCKS_PER_SLICE - 1;

	g_nvmeTask.status = NVME_TASK_RUNNING;
	queue_host_command(&fifo, 1, dwords, 2);

	FTL_TEST_RUN_UNTIL_ESCAPE(nvme_main());

	TEST_ASSERT_EQUAL_UINT(REQ_SLOT_TAG_NONE, sliceReqQ.headReq);
	dma.dword[0] = mock_io_last_write(HOST_DMA_CMD_FIFO_REG_ADDR, &found);
	TEST_ASSERT_TRUE(found);
	dma.dword[3] = mock_io_last_write(HOST_DMA_CMD_FIFO_REG_ADDR + 12, &found);
	TEST_ASSERT_TRUE(found);
	TEST_ASSERT_EQUAL_UINT(HOST_DMA_AUTO_TYPE, dma.dmaType);
	TEST_ASSERT_EQUAL_UINT(HOST_DMA_RX_DIRECTION, dma.dmaDirection);
	TEST_ASSERT_EQUAL_UINT(TEST_SLOT_TAG, dma.cmdSlotTag);
}

static void test_running_routes_queue_zero_to_admin_handler(void)
{
	cmd_fifo_t fifo;
	unsigned int dwords[NVME_CMD_DWORDS];
	NVME_ADMIN_COMMAND *admin = (NVME_ADMIN_COMMAND *)dwords;
	ADMIN_SET_FEATURES_DW10 dw10;
	NVME_CPL_FIFO_REG cpl;
	int found;

	memset(dwords, 0, sizeof(dwords));
	admin->OPC = ADMIN_SET_FEATURES;
	dw10.dword = 0;
	dw10.FID = VOLATILE_WRITE_CACHE;
	admin->dword10 = dw10.dword;
	admin->dword11 = 1;

	g_nvmeTask.status = NVME_TASK_RUNNING;
	g_nvmeTask.cacheEn = 0;
	queue_host_command(&fifo, 0, dwords, 2);

	FTL_TEST_RUN_UNTIL_ESCAPE(nvme_main());

	TEST_ASSERT_EQUAL_UINT(1, g_nvmeTask.cacheEn);
	TEST_ASSERT_EQUAL_UINT(REQ_SLOT_TAG_NONE, sliceReqQ.headReq);
	cpl.dword[2] = mock_io_last_write(CPL_FIFO_DWORD2_ADDR, &found);
	TEST_ASSERT_TRUE(found);
	TEST_ASSERT_EQUAL_UINT(TEST_SLOT_TAG, cpl.cmdSlotTag);
	TEST_ASSERT_EQUAL_UINT(AUTO_CPL_TYPE, cpl.cplType);
	TEST_ASSERT_EQUAL_UINT(0, cpl.statusFieldWord);
}

/* --- NVME_TASK_SHUTDOWN --------------------------------------------------- */

static void test_shutdown_waits_while_cc_shn_clear(void)
{
	counted_read_t c;

	g_nvmeTask.status = NVME_TASK_SHUTDOWN;
	set_status_reg(1, 0);
	escape_on_nth_read(NVME_STATUS_REG_ADDR, &c, 2);

	FTL_TEST_RUN_UNTIL_ESCAPE(nvme_main());

	TEST_ASSERT_EQUAL_UINT(NVME_TASK_SHUTDOWN, g_nvmeTask.status);
	TEST_ASSERT_EQUAL_size_t(0, mock_io_write_count_for(NVME_STATUS_REG_ADDR));
}

static void test_shutdown_tears_down_queues_and_reports_shutdown_complete(void)
{
	NVME_STATUS_REG status;
	NVME_ADMIN_QUEUE_SET_REG adminReg;

	g_nvmeTask.status = NVME_TASK_SHUTDOWN;
	g_nvmeTask.cacheEn = 1;
	set_status_reg(1, 1);
	ftl_test_set_printf_hook(book_grown_bad_then_escape_on_shutdown, NULL);

	FTL_TEST_RUN_UNTIL_ESCAPE(nvme_main());

	TEST_ASSERT_EQUAL_UINT(NVME_TASK_WAIT_RESET, g_nvmeTask.status);
	TEST_ASSERT_EQUAL_UINT(0, g_nvmeTask.cacheEn);

	/* SHST goes 1 (processing) then 2 (complete): two status writes. */
	TEST_ASSERT_EQUAL_size_t(2, mock_io_write_count_for(NVME_STATUS_REG_ADDR));
	status = last_status_write();
	TEST_ASSERT_EQUAL_UINT(2, status.cstsShst);

	TEST_ASSERT_EQUAL_size_t(IO_QUEUE_REG_WRITES, io_queue_reg_writes(NVME_IO_CQ_SET_REG_ADDR));
	TEST_ASSERT_EQUAL_size_t(IO_QUEUE_REG_WRITES, io_queue_reg_writes(NVME_IO_SQ_SET_REG_ADDR));
	adminReg = last_admin_queue_write();
	TEST_ASSERT_EQUAL_UINT(0, adminReg.sqValid);
	TEST_ASSERT_EQUAL_UINT(0, adminReg.cqValid);
	TEST_ASSERT_EQUAL_UINT(0, adminReg.cqIrqEn);

	/* Booked grown-bad update is flushed to NAND during shutdown. */
	TEST_ASSERT_GREATER_THAN_size_t(0, mock_nsc_count_op(MOCK_NSC_OP_PROGRAM));
}

/* --- NVME_TASK_WAIT_RESET ------------------------------------------------- */

static void test_wait_reset_waits_while_cc_en_still_set(void)
{
	counted_read_t c;

	g_nvmeTask.status = NVME_TASK_WAIT_RESET;
	set_status_reg(1, 0);
	escape_on_nth_read(NVME_STATUS_REG_ADDR, &c, 2);

	FTL_TEST_RUN_UNTIL_ESCAPE(nvme_main());

	TEST_ASSERT_EQUAL_UINT(NVME_TASK_WAIT_RESET, g_nvmeTask.status);
	TEST_ASSERT_EQUAL_size_t(0, mock_io_write_count_for(NVME_STATUS_REG_ADDR));
}

static void test_wait_reset_clears_ready_and_goes_idle_when_cc_en_cleared(void)
{
	NVME_STATUS_REG status;

	g_nvmeTask.status = NVME_TASK_WAIT_RESET;
	g_nvmeTask.cacheEn = 1;
	set_status_reg(0, 0);
	mock_io_set_reg(NVME_STATUS_REG_ADDR, 0x30); /* cstsRdy=1, cstsShst=2 */
	escape_when_firmware_prints("\r\nNVMe disable!!!\r\n");

	FTL_TEST_RUN_UNTIL_ESCAPE(nvme_main());

	TEST_ASSERT_EQUAL_UINT(NVME_TASK_IDLE, g_nvmeTask.status);
	TEST_ASSERT_EQUAL_UINT(0, g_nvmeTask.cacheEn);
	TEST_ASSERT_EQUAL_size_t(2, mock_io_write_count_for(NVME_STATUS_REG_ADDR));
	status = last_status_write();
	TEST_ASSERT_EQUAL_UINT(0, status.cstsShst);
	TEST_ASSERT_EQUAL_UINT(0, status.cstsRdy);
}

/* --- NVME_TASK_RESET ------------------------------------------------------ */

static void test_reset_disables_all_queues_and_goes_idle(void)
{
	NVME_STATUS_REG status;
	NVME_ADMIN_QUEUE_SET_REG adminReg;

	g_nvmeTask.status = NVME_TASK_RESET;
	g_nvmeTask.cacheEn = 1;
	mock_io_set_reg(NVME_STATUS_REG_ADDR, 0x11); /* ccEn=1, cstsRdy=1 */
	mock_io_set_reg(NVME_ADMIN_QUEUE_SET_REG_ADDR, 0x7);
	escape_when_firmware_prints("\r\nNVMe reset!!!\r\n");

	FTL_TEST_RUN_UNTIL_ESCAPE(nvme_main());

	TEST_ASSERT_EQUAL_UINT(NVME_TASK_IDLE, g_nvmeTask.status);
	TEST_ASSERT_EQUAL_UINT(0, g_nvmeTask.cacheEn);
	TEST_ASSERT_EQUAL_size_t(IO_QUEUE_REG_WRITES, io_queue_reg_writes(NVME_IO_CQ_SET_REG_ADDR));
	TEST_ASSERT_EQUAL_size_t(IO_QUEUE_REG_WRITES, io_queue_reg_writes(NVME_IO_SQ_SET_REG_ADDR));
	adminReg = last_admin_queue_write();
	TEST_ASSERT_EQUAL_UINT(0, adminReg.sqValid);
	TEST_ASSERT_EQUAL_UINT(0, adminReg.cqValid);
	TEST_ASSERT_EQUAL_UINT(0, adminReg.cqIrqEn);
	status = last_status_write();
	TEST_ASSERT_EQUAL_UINT(0, status.cstsRdy);
	TEST_ASSERT_EQUAL_UINT(0, status.cstsShst);
	TEST_ASSERT_EQUAL_UINT(1, status.ccEn); /* host-owned bit left untouched */
}

static void test_reset_marks_every_io_queue_invalid(void)
{
	unsigned int q;
	int found;

	g_nvmeTask.status = NVME_TASK_RESET;
	escape_when_firmware_prints("\r\nNVMe reset!!!\r\n");

	FTL_TEST_RUN_UNTIL_ESCAPE(nvme_main());

	for (q = 0; q < IO_QUEUE_COUNT; q++)
	{
		NVME_IO_SQ_SET_REG sq; memset(&sq, 0, sizeof(sq));
		NVME_IO_CQ_SET_REG cq; memset(&cq, 0, sizeof(cq));
		sq.dword[0] = mock_io_last_write(NVME_IO_SQ_SET_REG_ADDR + q * 8, &found);
		TEST_ASSERT_TRUE(found);
		TEST_ASSERT_EQUAL_UINT(0, sq.valid);
		cq.dword[0] = mock_io_last_write(NVME_IO_CQ_SET_REG_ADDR + q * 8, &found);
		TEST_ASSERT_TRUE(found);
		TEST_ASSERT_EQUAL_UINT(0, cq.valid);
	}
}

int main(void)
{
	UNITY_BEGIN();
	RUN_TEST(test_main_boots_ftl_before_entering_state_machine);
	RUN_TEST(test_wait_cc_en_stays_while_controller_disabled);
	RUN_TEST(test_wait_cc_en_enables_admin_queue_and_ready_when_cc_en_set);
	RUN_TEST(test_running_polls_command_fifo_when_no_command_pending);
	RUN_TEST(test_running_dispatches_io_read_and_schedules_low_level);
	RUN_TEST(test_running_io_write_allocates_buffer_and_requests_host_dma);
	RUN_TEST(test_running_routes_queue_zero_to_admin_handler);
	RUN_TEST(test_shutdown_waits_while_cc_shn_clear);
	RUN_TEST(test_shutdown_tears_down_queues_and_reports_shutdown_complete);
	RUN_TEST(test_wait_reset_waits_while_cc_en_still_set);
	RUN_TEST(test_wait_reset_clears_ready_and_goes_idle_when_cc_en_cleared);
	RUN_TEST(test_reset_disables_all_queues_and_goes_idle);
	RUN_TEST(test_reset_marks_every_io_queue_invalid);
	return UNITY_END();
}
