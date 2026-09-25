/* Unit tests for nvme/nvme_io_cmd.c and nvme/nvme_main.c. */
#include "unity.h"

#include <string.h>

#include "fw_test.h"
#include "ftl_config.h"
#include "memory_map.h"
#include "nvme/nvme.h"
#include "nvme/host_lld.h"
#include "nvme/nvme_io_cmd.h"
#include "nvme/nvme_main.h"
#include "request_allocation.h"
#include "request_format.h"
#include "request_transform.h"

extern volatile NVME_CONTEXT g_nvmeTask;

#define TEST_CAPACITY_BLOCKS 0x1000
#define CC_SHN_NORMAL_SHUTDOWN 0x1

void setUp(void)
{
	fw_test_reset();
	storageCapacity_L = TEST_CAPACITY_BLOCKS;
}
void tearDown(void) {}

static NVME_COMMAND make_io_cmd(unsigned char opc, unsigned short cmdSlotTag)
{
	NVME_COMMAND cmd;
	NVME_IO_COMMAND *io = (NVME_IO_COMMAND *)cmd.cmdDword;

	memset(&cmd, 0, sizeof(cmd));
	cmd.qID = 1;
	cmd.cmdSlotTag = cmdSlotTag;
	io->OPC = opc;
	io->PRP1[0] = 0x1000;
	io->PRP2[0] = 0x2000;
	return cmd;
}

static NVME_IO_COMMAND *io_of(NVME_COMMAND *cmd) { return (NVME_IO_COMMAND *)cmd->cmdDword; }

static void set_lba_range(NVME_COMMAND *cmd, unsigned int startLba, unsigned int nlbZeroBased)
{
	io_of(cmd)->dword10 = startLba;
	io_of(cmd)->dword11 = 0;
	io_of(cmd)->dword12 = nlbZeroBased & 0xFFFF;
}

/* ------------------------------------------------------------------------ */
/* handle_nvme_io_cmd                                                       */
/* ------------------------------------------------------------------------ */

static void test_smoke_flush_posts_successful_auto_completion(void)
{
	NVME_COMMAND cmd = make_io_cmd(IO_NVM_FLUSH, 5);

	handle_nvme_io_cmd(&cmd);

	TEST_ASSERT_EQUAL_UINT(1, mock_host_count(MOCK_HOST_SET_AUTO_NVME_CPL));
	TEST_ASSERT_EQUAL_UINT(5, mock_host_last(MOCK_HOST_SET_AUTO_NVME_CPL)->args[0]);
	TEST_ASSERT_EQUAL_UINT(0, mock_host_last(MOCK_HOST_SET_AUTO_NVME_CPL)->args[2]);
}

static void test_read_enqueues_slice_requests_tagged_with_command_slot(void)
{
	NVME_COMMAND cmd = make_io_cmd(IO_NVM_READ, 9);
	unsigned int reqSlotTag;

	fw_test_init_ftl();
	storageCapacity_L = TEST_CAPACITY_BLOCKS;
	set_lba_range(&cmd, 0, NVME_BLOCKS_PER_SLICE - 1);

	handle_nvme_io_cmd(&cmd);

	TEST_ASSERT_EQUAL_UINT(1, sliceReqQ.reqCnt);
	reqSlotTag = sliceReqQ.headReq;
	TEST_ASSERT_EQUAL_UINT(REQ_CODE_READ, reqPoolPtr->reqPool[reqSlotTag].reqCode);
	TEST_ASSERT_EQUAL_UINT(9, reqPoolPtr->reqPool[reqSlotTag].nvmeCmdSlotTag);
	TEST_ASSERT_EQUAL_UINT(0, mock_host_count(MOCK_HOST_SET_AUTO_NVME_CPL));
}

static void test_write_spanning_two_slices_enqueues_two_requests(void)
{
	NVME_COMMAND cmd = make_io_cmd(IO_NVM_WRITE, 10);

	fw_test_init_ftl();
	storageCapacity_L = TEST_CAPACITY_BLOCKS;
	set_lba_range(&cmd, NVME_BLOCKS_PER_SLICE - 1, 1);

	handle_nvme_io_cmd(&cmd);

	TEST_ASSERT_EQUAL_UINT(2, sliceReqQ.reqCnt);
	TEST_ASSERT_EQUAL_UINT(REQ_CODE_WRITE, reqPoolPtr->reqPool[sliceReqQ.headReq].reqCode);
	TEST_ASSERT_EQUAL_UINT(REQ_CODE_WRITE, reqPoolPtr->reqPool[sliceReqQ.tailReq].reqCode);
}

static void test_read_accepts_last_valid_lba(void)
{
	NVME_COMMAND cmd = make_io_cmd(IO_NVM_READ, 1);

	fw_test_init_ftl();
	storageCapacity_L = TEST_CAPACITY_BLOCKS;
	set_lba_range(&cmd, TEST_CAPACITY_BLOCKS - 1, 0);

	handle_nvme_io_cmd(&cmd);

	TEST_ASSERT_EQUAL_UINT(1, sliceReqQ.reqCnt);
}

static void test_read_rejects_lba_at_or_beyond_capacity(void)
{
	NVME_COMMAND cmd = make_io_cmd(IO_NVM_READ, 1);

	fw_test_init_ftl();
	storageCapacity_L = TEST_CAPACITY_BLOCKS;
	set_lba_range(&cmd, TEST_CAPACITY_BLOCKS, 0);

	FW_EXPECT_ASSERT(handle_nvme_io_cmd(&cmd));
	TEST_ASSERT_EQUAL_UINT(0, sliceReqQ.reqCnt);
}

static void test_read_rejects_nonzero_high_lba_dword(void)
{
	NVME_COMMAND cmd = make_io_cmd(IO_NVM_READ, 1);

	set_lba_range(&cmd, 0, 0);
	io_of(&cmd)->dword11 = 1;

	FW_EXPECT_ASSERT(handle_nvme_io_cmd(&cmd));
}

static void test_read_rejects_unaligned_prp_entries(void)
{
	NVME_COMMAND cmd = make_io_cmd(IO_NVM_READ, 1);

	set_lba_range(&cmd, 0, 0);
	io_of(&cmd)->PRP1[0] = 0x1004;
	FW_EXPECT_ASSERT(handle_nvme_io_cmd(&cmd));

	io_of(&cmd)->PRP1[0] = 0x1000;
	io_of(&cmd)->PRP2[0] = 0x2001;
	FW_EXPECT_ASSERT(handle_nvme_io_cmd(&cmd));
}

static void test_read_rejects_prp_above_36_bit_window(void)
{
	NVME_COMMAND cmd = make_io_cmd(IO_NVM_READ, 1);

	set_lba_range(&cmd, 0, 0);
	io_of(&cmd)->PRP1[1] = 0x10;
	FW_EXPECT_ASSERT(handle_nvme_io_cmd(&cmd));

	io_of(&cmd)->PRP1[1] = 0xF;
	io_of(&cmd)->PRP2[1] = 0x10;
	FW_EXPECT_ASSERT(handle_nvme_io_cmd(&cmd));
}

static void test_write_rejects_lba_beyond_capacity(void)
{
	NVME_COMMAND cmd = make_io_cmd(IO_NVM_WRITE, 1);

	set_lba_range(&cmd, TEST_CAPACITY_BLOCKS + 5, 0);

	FW_EXPECT_ASSERT(handle_nvme_io_cmd(&cmd));
}

static void test_write_rejects_unaligned_prp_and_high_dword(void)
{
	NVME_COMMAND cmd = make_io_cmd(IO_NVM_WRITE, 1);

	set_lba_range(&cmd, 0, 0);
	io_of(&cmd)->PRP2[0] = 0x2008;
	FW_EXPECT_ASSERT(handle_nvme_io_cmd(&cmd));

	io_of(&cmd)->PRP2[0] = 0x2000;
	io_of(&cmd)->PRP2[1] = 0x10;
	FW_EXPECT_ASSERT(handle_nvme_io_cmd(&cmd));

	io_of(&cmd)->PRP2[1] = 0;
	io_of(&cmd)->dword11 = 0x5;
	FW_EXPECT_ASSERT(handle_nvme_io_cmd(&cmd));
}

static void test_unsupported_io_opcodes_assert(void)
{
	static const unsigned char opcodes[] = {IO_NVM_WRITE_UNCORRECTABLE, IO_NVM_COMPARE,
			IO_NVM_DATASET_MANAGEMENT, 0x7F};
	unsigned int i;

	for (i = 0; i < sizeof(opcodes) / sizeof(opcodes[0]); i++) {
		NVME_COMMAND cmd = make_io_cmd(opcodes[i], 1);

		FW_EXPECT_ASSERT(handle_nvme_io_cmd(&cmd));
	}
	TEST_ASSERT_EQUAL_UINT(0, mock_host_call_count());
}

/* ------------------------------------------------------------------------ */
/* nvme_main state machine                                                  */
/* ------------------------------------------------------------------------ */

static void exit_after_ready(mock_host_call_kind_t kind)
{
	if (kind == MOCK_HOST_SET_NVME_CSTS_RDY)
		fw_loop_exit();
}

static void exit_when_no_more_commands(mock_host_call_kind_t kind)
{
	if (kind == MOCK_HOST_GET_NVME_CMD && mock_host_pending_cmds() == 0)
		fw_loop_exit();
}

static void push_cmd(unsigned short qID, unsigned short cmdSlotTag, const NVME_COMMAND *cmd)
{
	mock_host_nvme_cmd_t pending;

	memset(&pending, 0, sizeof(pending));
	pending.qID = qID;
	pending.cmdSlotTag = cmdSlotTag;
	memcpy(pending.cmdDword, cmd->cmdDword, sizeof(pending.cmdDword));
	mock_host_push_cmd(&pending);
}

static void test_smoke_nvme_main_enables_controller_when_cc_en_set(void)
{
	g_nvmeTask.status = NVME_TASK_WAIT_CC_EN;
	mock_host_set_cc_en(1);
	mock_host_set_hook(exit_after_ready);

	FW_RUN_UNTIL_LOOP_EXIT(nvme_main());

	TEST_ASSERT_EQUAL_UINT(1, mock_host_count(MOCK_HOST_SET_NVME_ADMIN_QUEUE));
	TEST_ASSERT_EQUAL_UINT(1, mock_host_count(MOCK_HOST_SET_NVME_CSTS_RDY));
	TEST_ASSERT_EQUAL_UINT(1, mock_host_last(MOCK_HOST_SET_NVME_CSTS_RDY)->args[0]);
}

static void exit_after_second_cc_en_poll(mock_host_call_kind_t kind)
{
	if (kind == MOCK_HOST_CHECK_NVME_CC_EN && mock_host_count(MOCK_HOST_CHECK_NVME_CC_EN) >= 2)
		fw_loop_exit();
}

static void test_nvme_main_keeps_waiting_while_cc_en_clear(void)
{
	g_nvmeTask.status = NVME_TASK_WAIT_CC_EN;
	mock_host_set_cc_en(0);
	mock_host_set_hook(exit_after_second_cc_en_poll);

	FW_RUN_UNTIL_LOOP_EXIT(nvme_main());

	TEST_ASSERT_EQUAL_UINT(0, mock_host_count(MOCK_HOST_SET_NVME_CSTS_RDY));
	TEST_ASSERT_EQUAL_UINT(NVME_TASK_WAIT_CC_EN, g_nvmeTask.status);
}

static void test_nvme_main_dispatches_admin_command_from_queue_zero(void)
{
	NVME_COMMAND cmd;
	NVME_ADMIN_COMMAND *admin = (NVME_ADMIN_COMMAND *)cmd.cmdDword;

	memset(&cmd, 0, sizeof(cmd));
	admin->OPC = ADMIN_SET_FEATURES;
	admin->dword10 = VOLATILE_WRITE_CACHE;
	admin->dword11 = 1;
	push_cmd(0, 17, &cmd);

	g_nvmeTask.status = NVME_TASK_RUNNING;
	mock_host_set_hook(exit_when_no_more_commands);

	FW_RUN_UNTIL_LOOP_EXIT(nvme_main());

	TEST_ASSERT_EQUAL_UINT(1, g_nvmeTask.cacheEn);
	TEST_ASSERT_EQUAL_UINT(1, mock_host_count(MOCK_HOST_SET_AUTO_NVME_CPL));
	TEST_ASSERT_EQUAL_UINT(17, mock_host_last(MOCK_HOST_SET_AUTO_NVME_CPL)->args[0]);
}

static void test_nvme_main_dispatches_io_command_and_issues_dma(void)
{
	NVME_COMMAND cmd = make_io_cmd(IO_NVM_WRITE, 3);

	set_lba_range(&cmd, 0, 0);
	push_cmd(1, 3, &cmd);

	g_nvmeTask.status = NVME_TASK_RUNNING;
	mock_host_set_hook(exit_when_no_more_commands);

	FW_RUN_UNTIL_LOOP_EXIT(nvme_main());

	TEST_ASSERT_EQUAL_UINT(0, sliceReqQ.reqCnt);
	TEST_ASSERT_EQUAL_UINT(1, mock_host_count(MOCK_HOST_SET_AUTO_RX_DMA));
	TEST_ASSERT_EQUAL_UINT(3, mock_host_last(MOCK_HOST_SET_AUTO_RX_DMA)->args[0]);
}

static void test_nvme_main_shutdown_tears_down_queues_and_waits_for_reset(void)
{
	NVME_STATUS_REG reg;

	memset(&reg, 0, sizeof(reg));
	reg.ccEn = 1;
	reg.ccShn = CC_SHN_NORMAL_SHUTDOWN;
	mock_io_set_reg(NVME_STATUS_REG_ADDR, reg.dword);

	g_nvmeTask.status = NVME_TASK_SHUTDOWN;
	g_nvmeTask.cacheEn = 1;
	mock_host_set_cc_en(1);
	mock_host_set_hook(exit_after_second_cc_en_poll);

	FW_RUN_UNTIL_LOOP_EXIT(nvme_main());

	TEST_ASSERT_EQUAL_UINT(2, mock_host_count(MOCK_HOST_SET_NVME_CSTS_SHST));
	TEST_ASSERT_EQUAL_UINT(1, mock_host_call_at(0)->args[0]);
	TEST_ASSERT_EQUAL_UINT(2, mock_host_last(MOCK_HOST_SET_NVME_CSTS_SHST)->args[0]);
	TEST_ASSERT_EQUAL_UINT(8, mock_host_count(MOCK_HOST_SET_IO_CQ));
	TEST_ASSERT_EQUAL_UINT(8, mock_host_count(MOCK_HOST_SET_IO_SQ));
	TEST_ASSERT_EQUAL_UINT(0, mock_host_last(MOCK_HOST_SET_IO_SQ)->args[1]);
	TEST_ASSERT_EQUAL_UINT(0, mock_host_last(MOCK_HOST_SET_NVME_ADMIN_QUEUE)->args[0]);
	TEST_ASSERT_EQUAL_UINT(0, g_nvmeTask.cacheEn);
	TEST_ASSERT_EQUAL_UINT(NVME_TASK_WAIT_RESET, g_nvmeTask.status);
}

static int exit_on_second_status_read(unsigned int addr, unsigned int *value)
{
	(void)value;
	if (addr == NVME_STATUS_REG_ADDR && mock_io_read_count(addr) >= 1)
		fw_loop_exit();
	return 0;
}

static void test_nvme_main_shutdown_ignores_cleared_shn(void)
{
	mock_io_set_reg(NVME_STATUS_REG_ADDR, 0);
	mock_io_set_read_hook(exit_on_second_status_read);
	g_nvmeTask.status = NVME_TASK_SHUTDOWN;

	FW_RUN_UNTIL_LOOP_EXIT(nvme_main());

	TEST_ASSERT_EQUAL_UINT(0, mock_host_count(MOCK_HOST_SET_NVME_CSTS_SHST));
	TEST_ASSERT_EQUAL_UINT(NVME_TASK_SHUTDOWN, g_nvmeTask.status);
}

static void test_nvme_main_wait_reset_goes_idle_once_cc_en_clears(void)
{
	g_nvmeTask.status = NVME_TASK_WAIT_RESET;
	g_nvmeTask.cacheEn = 1;
	mock_host_set_cc_en(0);
	mock_host_set_hook(exit_after_ready);

	FW_RUN_UNTIL_LOOP_EXIT(nvme_main());

	TEST_ASSERT_EQUAL_UINT(0, mock_host_last(MOCK_HOST_SET_NVME_CSTS_SHST)->args[0]);
	TEST_ASSERT_EQUAL_UINT(0, mock_host_last(MOCK_HOST_SET_NVME_CSTS_RDY)->args[0]);
	TEST_ASSERT_EQUAL_UINT(0, g_nvmeTask.cacheEn);
}

static void test_nvme_main_wait_reset_holds_while_cc_en_set(void)
{
	g_nvmeTask.status = NVME_TASK_WAIT_RESET;
	mock_host_set_cc_en(1);
	mock_host_set_hook(exit_after_second_cc_en_poll);

	FW_RUN_UNTIL_LOOP_EXIT(nvme_main());

	TEST_ASSERT_EQUAL_UINT(0, mock_host_count(MOCK_HOST_SET_NVME_CSTS_RDY));
	TEST_ASSERT_EQUAL_UINT(NVME_TASK_WAIT_RESET, g_nvmeTask.status);
}

static void test_nvme_main_reset_clears_all_queues_and_goes_idle(void)
{
	g_nvmeTask.status = NVME_TASK_RESET;
	g_nvmeTask.cacheEn = 1;
	mock_host_set_hook(exit_after_ready);

	FW_RUN_UNTIL_LOOP_EXIT(nvme_main());

	TEST_ASSERT_EQUAL_UINT(8, mock_host_count(MOCK_HOST_SET_IO_CQ));
	TEST_ASSERT_EQUAL_UINT(8, mock_host_count(MOCK_HOST_SET_IO_SQ));
	TEST_ASSERT_EQUAL_UINT(1, mock_host_count(MOCK_HOST_SET_NVME_ADMIN_QUEUE));
	TEST_ASSERT_EQUAL_UINT(0, mock_host_last(MOCK_HOST_SET_NVME_CSTS_SHST)->args[0]);
	TEST_ASSERT_EQUAL_UINT(0, mock_host_last(MOCK_HOST_SET_NVME_CSTS_RDY)->args[0]);
	TEST_ASSERT_EQUAL_UINT(0, g_nvmeTask.cacheEn);
}

int main(void)
{
	UNITY_BEGIN();
	RUN_TEST(test_smoke_flush_posts_successful_auto_completion);
	RUN_TEST(test_read_enqueues_slice_requests_tagged_with_command_slot);
	RUN_TEST(test_write_spanning_two_slices_enqueues_two_requests);
	RUN_TEST(test_read_accepts_last_valid_lba);
	RUN_TEST(test_read_rejects_lba_at_or_beyond_capacity);
	RUN_TEST(test_read_rejects_nonzero_high_lba_dword);
	RUN_TEST(test_read_rejects_unaligned_prp_entries);
	RUN_TEST(test_read_rejects_prp_above_36_bit_window);
	RUN_TEST(test_write_rejects_lba_beyond_capacity);
	RUN_TEST(test_write_rejects_unaligned_prp_and_high_dword);
	RUN_TEST(test_unsupported_io_opcodes_assert);
	RUN_TEST(test_smoke_nvme_main_enables_controller_when_cc_en_set);
	RUN_TEST(test_nvme_main_keeps_waiting_while_cc_en_clear);
	RUN_TEST(test_nvme_main_dispatches_admin_command_from_queue_zero);
	RUN_TEST(test_nvme_main_dispatches_io_command_and_issues_dma);
	RUN_TEST(test_nvme_main_shutdown_tears_down_queues_and_waits_for_reset);
	RUN_TEST(test_nvme_main_shutdown_ignores_cleared_shn);
	RUN_TEST(test_nvme_main_wait_reset_goes_idle_once_cc_en_clears);
	RUN_TEST(test_nvme_main_wait_reset_holds_while_cc_en_set);
	RUN_TEST(test_nvme_main_reset_clears_all_queues_and_goes_idle);
	return UNITY_END();
}
