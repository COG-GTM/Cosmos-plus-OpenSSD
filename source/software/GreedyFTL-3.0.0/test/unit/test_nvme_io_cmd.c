/* Unit tests for nvme/nvme_io_cmd.c and nvme/nvme_main.c. */
#include "unity.h"

#include <string.h>

#include "fw_test.h"
#include "memory_map.h"
#include "nvme/nvme.h"
#include "nvme/nvme_io_cmd.h"
#include "nvme/nvme_main.h"

extern volatile NVME_CONTEXT g_nvmeTask;

void setUp(void) { fw_test_reset(); }
void tearDown(void) {}

static NVME_COMMAND make_io_cmd(unsigned char opc, unsigned short cmdSlotTag)
{
	NVME_COMMAND cmd;
	NVME_IO_COMMAND *io = (NVME_IO_COMMAND *)cmd.cmdDword;

	memset(&cmd, 0, sizeof(cmd));
	cmd.qID = 1;
	cmd.cmdSlotTag = cmdSlotTag;
	io->OPC = opc;
	return cmd;
}

static void test_smoke_flush_posts_successful_auto_completion(void)
{
	NVME_COMMAND cmd = make_io_cmd(IO_NVM_FLUSH, 5);

	handle_nvme_io_cmd(&cmd);

	TEST_ASSERT_EQUAL_UINT(1, mock_host_count(MOCK_HOST_SET_AUTO_NVME_CPL));
	TEST_ASSERT_EQUAL_UINT(5, mock_host_last(MOCK_HOST_SET_AUTO_NVME_CPL)->args[0]);
	TEST_ASSERT_EQUAL_UINT(0, mock_host_last(MOCK_HOST_SET_AUTO_NVME_CPL)->args[2]);
}

static void exit_after_ready(mock_host_call_kind_t kind)
{
	if (kind == MOCK_HOST_SET_NVME_CSTS_RDY)
		fw_loop_exit();
}

static void test_smoke_nvme_main_enables_controller_when_cc_en_set(void)
{
	g_nvmeTask.status = NVME_TASK_WAIT_CC_EN;
	mock_host_set_cc_en(1);
	mock_host_set_hook(exit_after_ready);

	FW_RUN_UNTIL_LOOP_EXIT(nvme_main());

	TEST_ASSERT_EQUAL_UINT(1, mock_host_count(MOCK_HOST_SET_NVME_ADMIN_QUEUE));
	TEST_ASSERT_EQUAL_UINT(1, mock_host_count(MOCK_HOST_SET_NVME_CSTS_RDY));
}

int main(void)
{
	UNITY_BEGIN();
	RUN_TEST(test_smoke_flush_posts_successful_auto_completion);
	RUN_TEST(test_smoke_nvme_main_enables_controller_when_cc_en_set);
	return UNITY_END();
}
