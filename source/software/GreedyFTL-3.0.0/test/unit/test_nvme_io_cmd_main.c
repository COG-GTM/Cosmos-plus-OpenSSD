/* Unit tests for nvme/nvme_main.c: the NVMe controller task state machine. */
#include "unity.h"

#include <string.h>

#include "fw_test.h"
#include "memory_map.h"
#include "request_allocation.h"
#include "nvme/nvme.h"
#include "nvme/host_lld.h"
#include "nvme/nvme_admin_cmd.h"
#include "nvme/nvme_io_cmd.h"
#include "nvme/nvme_main.h"

extern volatile NVME_CONTEXT g_nvmeTask;

#define NUM_IO_QUEUES 8U
#define CC_SHN_NORMAL 0x1U
#define STATUS_REG_CC_SHN_SHIFT 1
#define POLLS_BEFORE_EXIT 3U

/* handle_nvme_admin_cmd is wrapped (test_nvme_io_cmd_main.wrap) so the
 * dispatch in nvme_main() can be observed without exercising the admin
 * module. */
static unsigned int admin_cmd_calls;
static NVME_COMMAND last_admin_cmd;

void __wrap_handle_nvme_admin_cmd(NVME_COMMAND *nvmeCmd)
{
	admin_cmd_calls++;
	last_admin_cmd = *nvmeCmd;
}

void setUp(void)
{
	fw_test_reset();
	admin_cmd_calls = 0;
	memset(&last_admin_cmd, 0, sizeof(last_admin_cmd));
}

void tearDown(void) {}

static void exit_on_csts_rdy(mock_host_call_kind_t kind)
{
	if (kind == MOCK_HOST_SET_NVME_CSTS_RDY)
		fw_loop_exit();
}

static void exit_after_polling_cc_en(mock_host_call_kind_t kind)
{
	if (kind == MOCK_HOST_CHECK_NVME_CC_EN && mock_host_count(kind) >= POLLS_BEFORE_EXIT)
		fw_loop_exit();
}

/* Leaves the loop once the command queue drained and the firmware polls again. */
static void exit_when_no_more_cmds(mock_host_call_kind_t kind)
{
	if (kind == MOCK_HOST_GET_NVME_CMD && mock_host_pending_cmds() == 0 &&
			mock_host_count(kind) >= 2)
		fw_loop_exit();
}

static void exit_on_first_cc_en_poll(mock_host_call_kind_t kind)
{
	if (kind == MOCK_HOST_CHECK_NVME_CC_EN)
		fw_loop_exit();
}

static int status_reg_polls;

static int count_status_reg_reads(unsigned int addr, unsigned int *value)
{
	(void)value;
	if (addr == NVME_STATUS_REG_ADDR && ++status_reg_polls >= (int)POLLS_BEFORE_EXIT)
		fw_loop_exit();
	return 0;
}

static mock_host_nvme_cmd_t make_cmd(unsigned short qID, unsigned short cmdSlotTag, unsigned char opc)
{
	mock_host_nvme_cmd_t cmd;
	NVME_IO_COMMAND *io = (NVME_IO_COMMAND *)cmd.cmdDword;

	memset(&cmd, 0, sizeof(cmd));
	cmd.qID = qID;
	cmd.cmdSlotTag = cmdSlotTag;
	cmd.cmdSeqNum = 0x1234;
	io->OPC = opc;
	return cmd;
}

static void assert_all_io_queues_invalidated(void)
{
	unsigned int qID;

	TEST_ASSERT_EQUAL_UINT(NUM_IO_QUEUES, mock_host_count(MOCK_HOST_SET_IO_CQ));
	TEST_ASSERT_EQUAL_UINT(NUM_IO_QUEUES, mock_host_count(MOCK_HOST_SET_IO_SQ));
	for (qID = 0; qID < NUM_IO_QUEUES; qID++) {
		unsigned int i;
		unsigned int sawCq = 0;
		unsigned int sawSq = 0;

		for (i = 0; i < mock_host_call_count(); i++) {
			const mock_host_call_t *call = mock_host_call_at(i);

			if (call->kind == MOCK_HOST_SET_IO_CQ && call->args[0] == qID) {
				TEST_ASSERT_EQUAL_UINT(0, call->args[1]);
				sawCq++;
			}
			if (call->kind == MOCK_HOST_SET_IO_SQ && call->args[0] == qID) {
				TEST_ASSERT_EQUAL_UINT(0, call->args[1]);
				sawSq++;
			}
		}
		TEST_ASSERT_EQUAL_UINT(1, sawCq);
		TEST_ASSERT_EQUAL_UINT(1, sawSq);
	}
}

/* ---- start-up ---- */

static void test_main_initialises_ftl_before_serving_host(void)
{
	g_nvmeTask.status = NVME_TASK_WAIT_CC_EN;
	mock_host_set_hook(exit_after_polling_cc_en);

	FW_RUN_UNTIL_LOOP_EXIT(nvme_main());

	/* InitFTL() scans the NAND: reset + status reads on every way. */
	TEST_ASSERT_GREATER_THAN_UINT(0, mock_nsc_count_cmd(V2FCommand_Reset));
	TEST_ASSERT_NOT_EQUAL(REQ_SLOT_TAG_NONE, freeReqQ.headReq);
}

/* ---- NVME_TASK_WAIT_CC_EN ---- */

static void test_wait_cc_en_keeps_polling_while_cc_en_clear(void)
{
	g_nvmeTask.status = NVME_TASK_WAIT_CC_EN;
	mock_host_set_cc_en(0);
	mock_host_set_hook(exit_after_polling_cc_en);

	FW_RUN_UNTIL_LOOP_EXIT(nvme_main());

	TEST_ASSERT_EQUAL_UINT(POLLS_BEFORE_EXIT, mock_host_count(MOCK_HOST_CHECK_NVME_CC_EN));
	TEST_ASSERT_EQUAL_UINT(0, mock_host_count(MOCK_HOST_SET_NVME_ADMIN_QUEUE));
	TEST_ASSERT_EQUAL_UINT(0, mock_host_count(MOCK_HOST_SET_NVME_CSTS_RDY));
	TEST_ASSERT_EQUAL_UINT(NVME_TASK_WAIT_CC_EN, g_nvmeTask.status);
}

static void test_wait_cc_en_enables_admin_queue_and_ready_when_cc_en_set(void)
{
	g_nvmeTask.status = NVME_TASK_WAIT_CC_EN;
	mock_host_set_cc_en(1);
	mock_host_set_hook(exit_on_csts_rdy);

	FW_RUN_UNTIL_LOOP_EXIT(nvme_main());

	TEST_ASSERT_EQUAL_UINT(1, mock_host_count(MOCK_HOST_SET_NVME_ADMIN_QUEUE));
	TEST_ASSERT_EQUAL_UINT(1, mock_host_last(MOCK_HOST_SET_NVME_ADMIN_QUEUE)->args[0]);
	TEST_ASSERT_EQUAL_UINT(1, mock_host_last(MOCK_HOST_SET_NVME_ADMIN_QUEUE)->args[1]);
	TEST_ASSERT_EQUAL_UINT(1, mock_host_last(MOCK_HOST_SET_NVME_ADMIN_QUEUE)->args[2]);
	TEST_ASSERT_EQUAL_UINT(1, mock_host_count(MOCK_HOST_SET_NVME_CSTS_RDY));
	TEST_ASSERT_EQUAL_UINT(1, mock_host_last(MOCK_HOST_SET_NVME_CSTS_RDY)->args[0]);
}

static void test_wait_cc_en_transitions_to_running(void)
{
	g_nvmeTask.status = NVME_TASK_WAIT_CC_EN;
	mock_host_set_cc_en(1);
	mock_host_set_hook(exit_when_no_more_cmds);

	FW_RUN_UNTIL_LOOP_EXIT(nvme_main());

	TEST_ASSERT_EQUAL_UINT(NVME_TASK_RUNNING, g_nvmeTask.status);
	TEST_ASSERT_EQUAL_UINT(1, mock_host_count(MOCK_HOST_CHECK_NVME_CC_EN));
}

/* ---- NVME_TASK_RUNNING ---- */

static void test_running_polls_for_commands_when_queue_empty(void)
{
	g_nvmeTask.status = NVME_TASK_RUNNING;
	mock_host_set_hook(exit_when_no_more_cmds);

	FW_RUN_UNTIL_LOOP_EXIT(nvme_main());

	TEST_ASSERT_EQUAL_UINT(2, mock_host_count(MOCK_HOST_GET_NVME_CMD));
	TEST_ASSERT_EQUAL_UINT(0, admin_cmd_calls);
	TEST_ASSERT_EQUAL_UINT(0, mock_host_count(MOCK_HOST_SET_AUTO_NVME_CPL));
	TEST_ASSERT_EQUAL_UINT(NVME_TASK_RUNNING, g_nvmeTask.status);
}

static void test_running_dispatches_admin_queue_commands_to_admin_handler(void)
{
	mock_host_nvme_cmd_t cmd = make_cmd(0, 7, ADMIN_IDENTIFY);

	g_nvmeTask.status = NVME_TASK_RUNNING;
	mock_host_push_cmd(&cmd);
	mock_host_set_hook(exit_when_no_more_cmds);

	FW_RUN_UNTIL_LOOP_EXIT(nvme_main());

	TEST_ASSERT_EQUAL_UINT(1, admin_cmd_calls);
	TEST_ASSERT_EQUAL_UINT(0, last_admin_cmd.qID);
	TEST_ASSERT_EQUAL_UINT(7, last_admin_cmd.cmdSlotTag);
	TEST_ASSERT_EQUAL_HEX32(0x1234, last_admin_cmd.cmdSeqNum);
	TEST_ASSERT_EQUAL_UINT(ADMIN_IDENTIFY, ((NVME_ADMIN_COMMAND *)last_admin_cmd.cmdDword)->OPC);
	TEST_ASSERT_EQUAL_UINT(0, mock_host_count(MOCK_HOST_SET_AUTO_NVME_CPL));
}

static void test_running_dispatches_io_queue_flush_to_io_handler(void)
{
	mock_host_nvme_cmd_t cmd = make_cmd(1, 4, IO_NVM_FLUSH);

	g_nvmeTask.status = NVME_TASK_RUNNING;
	mock_host_push_cmd(&cmd);
	mock_host_set_hook(exit_when_no_more_cmds);

	FW_RUN_UNTIL_LOOP_EXIT(nvme_main());

	TEST_ASSERT_EQUAL_UINT(0, admin_cmd_calls);
	TEST_ASSERT_EQUAL_UINT(1, mock_host_count(MOCK_HOST_SET_AUTO_NVME_CPL));
	TEST_ASSERT_EQUAL_UINT(4, mock_host_last(MOCK_HOST_SET_AUTO_NVME_CPL)->args[0]);
}

static void test_running_io_read_is_translated_and_issued_to_nand(void)
{
	mock_host_nvme_cmd_t cmd = make_cmd(1, 2, IO_NVM_READ);

	g_nvmeTask.status = NVME_TASK_RUNNING;
	mock_host_push_cmd(&cmd);
	mock_host_set_hook(exit_when_no_more_cmds);

	FW_RUN_UNTIL_LOOP_EXIT(nvme_main());

	/* ReqTransSliceToLowLevel() ran: the slice queue is drained ... */
	TEST_ASSERT_EQUAL_UINT(0, sliceReqQ.reqCnt);
	/* ... and the low-level scheduler pushed a read to the NAND controller. */
	TEST_ASSERT_GREATER_THAN_UINT(0, mock_nsc_count_cmd(V2FCommand_ReadPageTrigger));
}

static void test_running_io_write_streams_host_data_via_dma(void)
{
	mock_host_nvme_cmd_t cmd = make_cmd(1, 6, IO_NVM_WRITE);

	g_nvmeTask.status = NVME_TASK_RUNNING;
	mock_host_push_cmd(&cmd);
	mock_host_set_hook(exit_when_no_more_cmds);

	FW_RUN_UNTIL_LOOP_EXIT(nvme_main());

	TEST_ASSERT_EQUAL_UINT(0, sliceReqQ.reqCnt);
	/* nlb = 0 is a single 4 KiB block, so exactly one host-to-device DMA. */
	TEST_ASSERT_EQUAL_UINT(1, mock_host_count(MOCK_HOST_SET_AUTO_RX_DMA));
	TEST_ASSERT_EQUAL_UINT(6, mock_host_last(MOCK_HOST_SET_AUTO_RX_DMA)->args[0]);
}

static void test_running_serves_commands_in_arrival_order(void)
{
	mock_host_nvme_cmd_t admin = make_cmd(0, 1, ADMIN_GET_LOG_PAGE);
	mock_host_nvme_cmd_t flushA = make_cmd(1, 2, IO_NVM_FLUSH);
	mock_host_nvme_cmd_t flushB = make_cmd(2, 3, IO_NVM_FLUSH);

	g_nvmeTask.status = NVME_TASK_RUNNING;
	mock_host_push_cmd(&flushA);
	mock_host_push_cmd(&admin);
	mock_host_push_cmd(&flushB);
	mock_host_set_hook(exit_when_no_more_cmds);

	FW_RUN_UNTIL_LOOP_EXIT(nvme_main());

	TEST_ASSERT_EQUAL_UINT(1, admin_cmd_calls);
	TEST_ASSERT_EQUAL_UINT(2, mock_host_count(MOCK_HOST_SET_AUTO_NVME_CPL));
	TEST_ASSERT_EQUAL_UINT(2, mock_host_call_at(0)->args[0]);
	TEST_ASSERT_EQUAL_UINT(3, mock_host_last(MOCK_HOST_SET_AUTO_NVME_CPL)->args[0]);
	TEST_ASSERT_EQUAL_UINT(0, mock_host_pending_cmds());
}

/* ---- NVME_TASK_SHUTDOWN ---- */

static void test_shutdown_waits_while_cc_shn_is_clear(void)
{
	g_nvmeTask.status = NVME_TASK_SHUTDOWN;
	mock_io_set_reg(NVME_STATUS_REG_ADDR, 0);
	status_reg_polls = 0;
	mock_io_set_read_hook(count_status_reg_reads);

	FW_RUN_UNTIL_LOOP_EXIT(nvme_main());

	/* The read that triggers the exit is not logged, hence one less. */
	TEST_ASSERT_EQUAL_UINT(POLLS_BEFORE_EXIT - 1, mock_io_read_count(NVME_STATUS_REG_ADDR));
	TEST_ASSERT_EQUAL_UINT(0, mock_host_count(MOCK_HOST_SET_NVME_CSTS_SHST));
	TEST_ASSERT_EQUAL_UINT(NVME_TASK_SHUTDOWN, g_nvmeTask.status);
}

static void test_shutdown_on_cc_shn_tears_down_queues_and_reports_complete(void)
{
	g_nvmeTask.status = NVME_TASK_SHUTDOWN;
	g_nvmeTask.cacheEn = 1;
	mock_io_set_reg(NVME_STATUS_REG_ADDR, CC_SHN_NORMAL << STATUS_REG_CC_SHN_SHIFT);
	/* CC.EN still set: the task parks in WAIT_RESET, polling CC.EN. */
	mock_host_set_cc_en(1);
	mock_host_set_hook(exit_on_first_cc_en_poll);

	FW_RUN_UNTIL_LOOP_EXIT(nvme_main());

	TEST_ASSERT_EQUAL_UINT(2, mock_host_count(MOCK_HOST_SET_NVME_CSTS_SHST));
	TEST_ASSERT_EQUAL_UINT(MOCK_HOST_SET_NVME_CSTS_SHST, mock_host_call_at(0)->kind);
	TEST_ASSERT_EQUAL_UINT(1, mock_host_call_at(0)->args[0]);
	assert_all_io_queues_invalidated();
	TEST_ASSERT_EQUAL_UINT(1, mock_host_count(MOCK_HOST_SET_NVME_ADMIN_QUEUE));
	TEST_ASSERT_EQUAL_UINT(0, mock_host_last(MOCK_HOST_SET_NVME_ADMIN_QUEUE)->args[0]);
	TEST_ASSERT_EQUAL_UINT(0, mock_host_last(MOCK_HOST_SET_NVME_ADMIN_QUEUE)->args[1]);
	TEST_ASSERT_EQUAL_UINT(2, mock_host_last(MOCK_HOST_SET_NVME_CSTS_SHST)->args[0]);
	TEST_ASSERT_EQUAL_UINT(0, g_nvmeTask.cacheEn);
	TEST_ASSERT_EQUAL_UINT(NVME_TASK_WAIT_RESET, g_nvmeTask.status);
}

static void test_shutdown_then_cc_en_clear_disables_controller(void)
{
	g_nvmeTask.status = NVME_TASK_SHUTDOWN;
	mock_io_set_reg(NVME_STATUS_REG_ADDR, CC_SHN_NORMAL << STATUS_REG_CC_SHN_SHIFT);
	mock_host_set_cc_en(0);
	mock_host_set_hook(exit_on_csts_rdy);

	FW_RUN_UNTIL_LOOP_EXIT(nvme_main());

	/* Grown bad block table flush happened before leaving SHUTDOWN. */
	TEST_ASSERT_GREATER_THAN_UINT(0, mock_nsc_count_cmd(V2FCommand_ProgramPage));
	TEST_ASSERT_EQUAL_UINT(3, mock_host_count(MOCK_HOST_SET_NVME_CSTS_SHST));
	TEST_ASSERT_EQUAL_UINT(0, mock_host_last(MOCK_HOST_SET_NVME_CSTS_SHST)->args[0]);
	TEST_ASSERT_EQUAL_UINT(1, mock_host_count(MOCK_HOST_SET_NVME_CSTS_RDY));
	TEST_ASSERT_EQUAL_UINT(0, mock_host_last(MOCK_HOST_SET_NVME_CSTS_RDY)->args[0]);
	TEST_ASSERT_EQUAL_UINT(0, mock_host_pending_cmds());
}

/* ---- NVME_TASK_WAIT_RESET ---- */

static void test_wait_reset_keeps_polling_while_cc_en_set(void)
{
	g_nvmeTask.status = NVME_TASK_WAIT_RESET;
	mock_host_set_cc_en(1);
	mock_host_set_hook(exit_after_polling_cc_en);

	FW_RUN_UNTIL_LOOP_EXIT(nvme_main());

	TEST_ASSERT_EQUAL_UINT(POLLS_BEFORE_EXIT, mock_host_count(MOCK_HOST_CHECK_NVME_CC_EN));
	TEST_ASSERT_EQUAL_UINT(0, mock_host_count(MOCK_HOST_SET_NVME_CSTS_RDY));
	TEST_ASSERT_EQUAL_UINT(NVME_TASK_WAIT_RESET, g_nvmeTask.status);
}

static void test_wait_reset_clears_ready_and_shutdown_status_when_cc_en_clear(void)
{
	g_nvmeTask.status = NVME_TASK_WAIT_RESET;
	g_nvmeTask.cacheEn = 1;
	mock_host_set_cc_en(0);
	mock_host_set_hook(exit_on_csts_rdy);

	FW_RUN_UNTIL_LOOP_EXIT(nvme_main());

	TEST_ASSERT_EQUAL_UINT(1, mock_host_count(MOCK_HOST_SET_NVME_CSTS_SHST));
	TEST_ASSERT_EQUAL_UINT(0, mock_host_last(MOCK_HOST_SET_NVME_CSTS_SHST)->args[0]);
	TEST_ASSERT_EQUAL_UINT(0, mock_host_last(MOCK_HOST_SET_NVME_CSTS_RDY)->args[0]);
	TEST_ASSERT_EQUAL_UINT(0, g_nvmeTask.cacheEn);
	TEST_ASSERT_EQUAL_UINT(0, mock_host_count(MOCK_HOST_SET_IO_CQ));
}

/* ---- NVME_TASK_RESET ---- */

static void test_reset_invalidates_all_queues_and_goes_idle(void)
{
	g_nvmeTask.status = NVME_TASK_RESET;
	g_nvmeTask.cacheEn = 1;
	mock_host_set_hook(exit_on_csts_rdy);

	FW_RUN_UNTIL_LOOP_EXIT(nvme_main());

	assert_all_io_queues_invalidated();
	TEST_ASSERT_EQUAL_UINT(1, mock_host_count(MOCK_HOST_SET_NVME_ADMIN_QUEUE));
	TEST_ASSERT_EQUAL_UINT(0, mock_host_last(MOCK_HOST_SET_NVME_ADMIN_QUEUE)->args[0]);
	TEST_ASSERT_EQUAL_UINT(1, mock_host_count(MOCK_HOST_SET_NVME_CSTS_SHST));
	TEST_ASSERT_EQUAL_UINT(0, mock_host_last(MOCK_HOST_SET_NVME_CSTS_SHST)->args[0]);
	TEST_ASSERT_EQUAL_UINT(0, mock_host_last(MOCK_HOST_SET_NVME_CSTS_RDY)->args[0]);
	TEST_ASSERT_EQUAL_UINT(0, g_nvmeTask.cacheEn);
	TEST_ASSERT_EQUAL_UINT(0, mock_host_count(MOCK_HOST_CHECK_NVME_CC_EN));
}

static void test_reset_does_not_touch_host_command_queue(void)
{
	mock_host_nvme_cmd_t cmd = make_cmd(1, 1, IO_NVM_FLUSH);

	g_nvmeTask.status = NVME_TASK_RESET;
	mock_host_push_cmd(&cmd);
	mock_host_set_hook(exit_on_csts_rdy);

	FW_RUN_UNTIL_LOOP_EXIT(nvme_main());

	TEST_ASSERT_EQUAL_UINT(0, mock_host_count(MOCK_HOST_GET_NVME_CMD));
	TEST_ASSERT_EQUAL_UINT(1, mock_host_pending_cmds());
	TEST_ASSERT_EQUAL_UINT(0, mock_host_count(MOCK_HOST_SET_AUTO_NVME_CPL));
}

int main(void)
{
	UNITY_BEGIN();
	RUN_TEST(test_main_initialises_ftl_before_serving_host);

	RUN_TEST(test_wait_cc_en_keeps_polling_while_cc_en_clear);
	RUN_TEST(test_wait_cc_en_enables_admin_queue_and_ready_when_cc_en_set);
	RUN_TEST(test_wait_cc_en_transitions_to_running);

	RUN_TEST(test_running_polls_for_commands_when_queue_empty);
	RUN_TEST(test_running_dispatches_admin_queue_commands_to_admin_handler);
	RUN_TEST(test_running_dispatches_io_queue_flush_to_io_handler);
	RUN_TEST(test_running_io_read_is_translated_and_issued_to_nand);
	RUN_TEST(test_running_io_write_streams_host_data_via_dma);
	RUN_TEST(test_running_serves_commands_in_arrival_order);

	RUN_TEST(test_shutdown_waits_while_cc_shn_is_clear);
	RUN_TEST(test_shutdown_on_cc_shn_tears_down_queues_and_reports_complete);
	RUN_TEST(test_shutdown_then_cc_en_clear_disables_controller);

	RUN_TEST(test_wait_reset_keeps_polling_while_cc_en_set);
	RUN_TEST(test_wait_reset_clears_ready_and_shutdown_status_when_cc_en_clear);

	RUN_TEST(test_reset_invalidates_all_queues_and_goes_idle);
	RUN_TEST(test_reset_does_not_touch_host_command_queue);
	return UNITY_END();
}
