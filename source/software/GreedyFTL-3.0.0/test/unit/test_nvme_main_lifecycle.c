/* Unit tests for nvme/nvme_main.c: transitions into NVME_TASK_IDLE and the
 * background low-level request servicing that runs between host commands.
 *
 * On target the host controller interrupt handler (host_lld.c) changes
 * g_nvmeTask.status while nvme_main() is spinning. Here the mock hook plays
 * that role: it flips the status from inside a mocked host_lld call. */
#include "unity.h"

#include <string.h>

#include "fw_test.h"
#include "memory_map.h"
#include "request_allocation.h"
#include "nvme/nvme.h"
#include "nvme/host_lld.h"
#include "nvme/nvme_io_cmd.h"
#include "nvme/nvme_main.h"

extern volatile NVME_CONTEXT g_nvmeTask;

#define NUM_IO_QUEUES 8U
#define DMA_POLLS_BEFORE_EXIT 3U
#define CMD_POLLS_BEFORE_EXIT 3U
#define WRITE_SLOT_TAG 9U

/* Status injected by the hook on the first DMA-completion poll, or 0 for none. */
static unsigned int status_to_inject;
static unsigned int dma_polls_seen;
static unsigned int dma_polls_until_done;

void setUp(void)
{
	fw_test_reset();
	status_to_inject = 0;
	dma_polls_seen = 0;
	dma_polls_until_done = 0;
}

void tearDown(void) {}

static mock_host_nvme_cmd_t make_write_cmd(unsigned short cmdSlotTag)
{
	mock_host_nvme_cmd_t cmd;
	NVME_IO_COMMAND *io = (NVME_IO_COMMAND *)cmd.cmdDword;

	memset(&cmd, 0, sizeof(cmd));
	cmd.qID = 1;
	cmd.cmdSlotTag = cmdSlotTag;
	cmd.cmdSeqNum = 0x55;
	io->OPC = IO_NVM_WRITE;
	return cmd;
}

/* Keeps a write's RxDMA pending so nvmeDmaReqQ stays non-empty. */
static void push_write_with_pending_dma(void)
{
	mock_host_nvme_cmd_t cmd = make_write_cmd(WRITE_SLOT_TAG);

	g_nvmeTask.status = NVME_TASK_RUNNING;
	mock_host_set_partial_done(0);
	mock_host_push_cmd(&cmd);
}

/* Injects status_to_inject on the first DMA poll, exits on the Nth. */
static void inject_status_then_exit_on_dma_poll(mock_host_call_kind_t kind)
{
	if (kind != MOCK_HOST_CHECK_AUTO_RX_DMA_PARTIAL_DONE)
		return;
	dma_polls_seen++;
	if (dma_polls_seen == 1 && status_to_inject != 0)
		g_nvmeTask.status = status_to_inject;
	if (dma_polls_seen >= DMA_POLLS_BEFORE_EXIT)
		fw_loop_exit();
}

/* Marks the DMA done after dma_polls_until_done polls, then exits once the
 * firmware is back to polling the (empty) host command queue. */
static void complete_dma_then_exit_when_idle_polling(mock_host_call_kind_t kind)
{
	if (kind == MOCK_HOST_CHECK_AUTO_RX_DMA_PARTIAL_DONE) {
		dma_polls_seen++;
		if (dma_polls_seen >= dma_polls_until_done)
			mock_host_set_partial_done(1);
	}
	if (kind == MOCK_HOST_GET_NVME_CMD && mock_host_pending_cmds() == 0 &&
			dma_polls_seen >= dma_polls_until_done &&
			mock_host_count(kind) >= CMD_POLLS_BEFORE_EXIT)
		fw_loop_exit();
}

/* Parks the task in IDLE while a second host command arrives. */
static void go_idle_and_queue_cmd_then_exit_on_dma_poll(mock_host_call_kind_t kind)
{
	if (kind != MOCK_HOST_CHECK_AUTO_RX_DMA_PARTIAL_DONE)
		return;
	dma_polls_seen++;
	if (dma_polls_seen == 1) {
		mock_host_nvme_cmd_t cmd = make_write_cmd(WRITE_SLOT_TAG + 1);

		g_nvmeTask.status = NVME_TASK_IDLE;
		mock_host_push_cmd(&cmd);
	}
	if (dma_polls_seen >= DMA_POLLS_BEFORE_EXIT)
		fw_loop_exit();
}

static void assert_all_io_queues_invalidated(void)
{
	unsigned int i;
	unsigned int cqCount = 0;
	unsigned int sqCount = 0;

	for (i = 0; i < mock_host_call_count(); i++) {
		const mock_host_call_t *call = mock_host_call_at(i);

		if (call->kind == MOCK_HOST_SET_IO_CQ) {
			TEST_ASSERT_EQUAL_UINT(cqCount, call->args[0]);
			TEST_ASSERT_EQUAL_UINT(0, call->args[1]);
			cqCount++;
		}
		if (call->kind == MOCK_HOST_SET_IO_SQ) {
			TEST_ASSERT_EQUAL_UINT(sqCount, call->args[0]);
			TEST_ASSERT_EQUAL_UINT(0, call->args[1]);
			sqCount++;
		}
	}
	TEST_ASSERT_EQUAL_UINT(NUM_IO_QUEUES, cqCount);
	TEST_ASSERT_EQUAL_UINT(NUM_IO_QUEUES, sqCount);
}

/* ---- background low-level request servicing ---- */

static void test_running_polls_pending_dma_while_no_host_command(void)
{
	push_write_with_pending_dma();
	mock_host_set_hook(inject_status_then_exit_on_dma_poll);

	FW_RUN_UNTIL_LOOP_EXIT(nvme_main());

	TEST_ASSERT_EQUAL_UINT(1, mock_host_count(MOCK_HOST_SET_AUTO_RX_DMA));
	TEST_ASSERT_EQUAL_UINT(WRITE_SLOT_TAG, mock_host_last(MOCK_HOST_SET_AUTO_RX_DMA)->args[0]);
	TEST_ASSERT_EQUAL_UINT(DMA_POLLS_BEFORE_EXIT,
			mock_host_count(MOCK_HOST_CHECK_AUTO_RX_DMA_PARTIAL_DONE));
	TEST_ASSERT_NOT_EQUAL(REQ_SLOT_TAG_NONE, nvmeDmaReqQ.headReq);
	TEST_ASSERT_EQUAL_UINT(NVME_TASK_RUNNING, g_nvmeTask.status);
}

static void test_running_retires_dma_request_once_host_reports_done(void)
{
	push_write_with_pending_dma();
	dma_polls_until_done = 2;
	mock_host_set_hook(complete_dma_then_exit_when_idle_polling);

	FW_RUN_UNTIL_LOOP_EXIT(nvme_main());

	TEST_ASSERT_EQUAL_UINT(2, mock_host_count(MOCK_HOST_CHECK_AUTO_RX_DMA_PARTIAL_DONE));
	TEST_ASSERT_EQUAL_UINT(REQ_SLOT_TAG_NONE, nvmeDmaReqQ.headReq);
	TEST_ASSERT_EQUAL_UINT(0, mock_host_pending_cmds());
	TEST_ASSERT_EQUAL_UINT(NVME_TASK_RUNNING, g_nvmeTask.status);
}

static void test_running_skips_dma_poll_when_nothing_outstanding(void)
{
	mock_host_nvme_cmd_t cmd = make_write_cmd(WRITE_SLOT_TAG);

	g_nvmeTask.status = NVME_TASK_RUNNING;
	mock_host_set_partial_done(1);
	mock_host_push_cmd(&cmd);
	dma_polls_until_done = 1;
	mock_host_set_hook(complete_dma_then_exit_when_idle_polling);

	FW_RUN_UNTIL_LOOP_EXIT(nvme_main());

	/* Immediate DMA completion: one poll retires the request, none after. */
	TEST_ASSERT_EQUAL_UINT(1, mock_host_count(MOCK_HOST_CHECK_AUTO_RX_DMA_PARTIAL_DONE));
	TEST_ASSERT_EQUAL_UINT(REQ_SLOT_TAG_NONE, nvmeDmaReqQ.headReq);
	TEST_ASSERT_EQUAL_UINT(CMD_POLLS_BEFORE_EXIT, mock_host_count(MOCK_HOST_GET_NVME_CMD));
}

/* ---- NVME_TASK_WAIT_RESET -> NVME_TASK_IDLE ---- */

static void test_wait_reset_with_cc_en_clear_goes_idle(void)
{
	push_write_with_pending_dma();
	g_nvmeTask.cacheEn = 1;
	mock_host_set_cc_en(0);
	status_to_inject = NVME_TASK_WAIT_RESET;
	mock_host_set_hook(inject_status_then_exit_on_dma_poll);

	FW_RUN_UNTIL_LOOP_EXIT(nvme_main());

	TEST_ASSERT_EQUAL_UINT(NVME_TASK_IDLE, g_nvmeTask.status);
	TEST_ASSERT_EQUAL_UINT(0, g_nvmeTask.cacheEn);
	TEST_ASSERT_EQUAL_UINT(1, mock_host_count(MOCK_HOST_CHECK_NVME_CC_EN));
	TEST_ASSERT_EQUAL_UINT(1, mock_host_count(MOCK_HOST_SET_NVME_CSTS_SHST));
	TEST_ASSERT_EQUAL_UINT(0, mock_host_last(MOCK_HOST_SET_NVME_CSTS_SHST)->args[0]);
	TEST_ASSERT_EQUAL_UINT(1, mock_host_count(MOCK_HOST_SET_NVME_CSTS_RDY));
	TEST_ASSERT_EQUAL_UINT(0, mock_host_last(MOCK_HOST_SET_NVME_CSTS_RDY)->args[0]);
	/* WAIT_RESET does not touch the queues; that is RESET's job. */
	TEST_ASSERT_EQUAL_UINT(0, mock_host_count(MOCK_HOST_SET_IO_CQ));
	TEST_ASSERT_EQUAL_UINT(0, mock_host_count(MOCK_HOST_SET_NVME_ADMIN_QUEUE));
}

static void test_idle_after_wait_reset_still_services_pending_dma(void)
{
	push_write_with_pending_dma();
	mock_host_set_cc_en(0);
	status_to_inject = NVME_TASK_WAIT_RESET;
	mock_host_set_hook(inject_status_then_exit_on_dma_poll);

	FW_RUN_UNTIL_LOOP_EXIT(nvme_main());

	TEST_ASSERT_EQUAL_UINT(NVME_TASK_IDLE, g_nvmeTask.status);
	TEST_ASSERT_EQUAL_UINT(DMA_POLLS_BEFORE_EXIT, dma_polls_seen);
	/* RUNNING fetched the write and polled once more; IDLE never fetches. */
	TEST_ASSERT_EQUAL_UINT(2, mock_host_count(MOCK_HOST_GET_NVME_CMD));
	TEST_ASSERT_NOT_EQUAL(REQ_SLOT_TAG_NONE, nvmeDmaReqQ.headReq);
}

/* ---- NVME_TASK_RESET -> NVME_TASK_IDLE ---- */

static void test_reset_goes_idle_after_invalidating_queues(void)
{
	push_write_with_pending_dma();
	g_nvmeTask.cacheEn = 1;
	status_to_inject = NVME_TASK_RESET;
	mock_host_set_hook(inject_status_then_exit_on_dma_poll);

	FW_RUN_UNTIL_LOOP_EXIT(nvme_main());

	TEST_ASSERT_EQUAL_UINT(NVME_TASK_IDLE, g_nvmeTask.status);
	TEST_ASSERT_EQUAL_UINT(0, g_nvmeTask.cacheEn);
	assert_all_io_queues_invalidated();
	TEST_ASSERT_EQUAL_UINT(1, mock_host_count(MOCK_HOST_SET_NVME_ADMIN_QUEUE));
	TEST_ASSERT_EQUAL_UINT(0, mock_host_last(MOCK_HOST_SET_NVME_ADMIN_QUEUE)->args[0]);
	TEST_ASSERT_EQUAL_UINT(0, mock_host_last(MOCK_HOST_SET_NVME_CSTS_SHST)->args[0]);
	TEST_ASSERT_EQUAL_UINT(0, mock_host_last(MOCK_HOST_SET_NVME_CSTS_RDY)->args[0]);
	/* RESET never consults CC.EN. */
	TEST_ASSERT_EQUAL_UINT(0, mock_host_count(MOCK_HOST_CHECK_NVME_CC_EN));
}

static void test_reset_runs_exactly_once_then_stays_idle(void)
{
	push_write_with_pending_dma();
	status_to_inject = NVME_TASK_RESET;
	mock_host_set_hook(inject_status_then_exit_on_dma_poll);

	FW_RUN_UNTIL_LOOP_EXIT(nvme_main());

	/* Two more loop iterations ran in IDLE without re-invalidating queues. */
	TEST_ASSERT_EQUAL_UINT(DMA_POLLS_BEFORE_EXIT, dma_polls_seen);
	TEST_ASSERT_EQUAL_UINT(NUM_IO_QUEUES, mock_host_count(MOCK_HOST_SET_IO_CQ));
	TEST_ASSERT_EQUAL_UINT(NUM_IO_QUEUES, mock_host_count(MOCK_HOST_SET_IO_SQ));
	TEST_ASSERT_EQUAL_UINT(1, mock_host_count(MOCK_HOST_SET_NVME_CSTS_RDY));
	TEST_ASSERT_EQUAL_UINT(1, mock_host_count(MOCK_HOST_SET_NVME_CSTS_SHST));
	TEST_ASSERT_EQUAL_UINT(NVME_TASK_IDLE, g_nvmeTask.status);
}

/* ---- NVME_TASK_IDLE ---- */

static void test_idle_ignores_host_commands(void)
{
	push_write_with_pending_dma();
	mock_host_set_cc_en(1);
	mock_host_set_hook(go_idle_and_queue_cmd_then_exit_on_dma_poll);

	FW_RUN_UNTIL_LOOP_EXIT(nvme_main());

	TEST_ASSERT_EQUAL_UINT(NVME_TASK_IDLE, g_nvmeTask.status);
	TEST_ASSERT_EQUAL_UINT(DMA_POLLS_BEFORE_EXIT, dma_polls_seen);
	/* The second write arrived after going IDLE and is never fetched. */
	TEST_ASSERT_EQUAL_UINT(2, mock_host_count(MOCK_HOST_GET_NVME_CMD));
	TEST_ASSERT_EQUAL_UINT(1, mock_host_pending_cmds());
	TEST_ASSERT_EQUAL_UINT(1, mock_host_count(MOCK_HOST_SET_AUTO_RX_DMA));
	TEST_ASSERT_EQUAL_UINT(0, mock_host_count(MOCK_HOST_CHECK_NVME_CC_EN));
	TEST_ASSERT_EQUAL_UINT(0, mock_host_count(MOCK_HOST_SET_NVME_CSTS_RDY));
}

int main(void)
{
	UNITY_BEGIN();
	RUN_TEST(test_running_polls_pending_dma_while_no_host_command);
	RUN_TEST(test_running_retires_dma_request_once_host_reports_done);
	RUN_TEST(test_running_skips_dma_poll_when_nothing_outstanding);

	RUN_TEST(test_wait_reset_with_cc_en_clear_goes_idle);
	RUN_TEST(test_idle_after_wait_reset_still_services_pending_dma);

	RUN_TEST(test_reset_goes_idle_after_invalidating_queues);
	RUN_TEST(test_reset_runs_exactly_once_then_stays_idle);

	RUN_TEST(test_idle_ignores_host_commands);
	return UNITY_END();
}
