/*
 * Links the real nvme/host_lld.c against mock_io to prove that IO_READ32 /
 * IO_WRITE32 register traffic and host DMA bookkeeping are capturable.
 */
#include "unity.h"

#include <string.h>

#include "fw_assert.h"
#include "fw_memory.h"
#include "mock_io.h"
#include "nvme/host_lld.h"
#include "nvme/nvme.h"
#include "request_transform.h"

NVME_CONTEXT g_nvmeTask;

void setUp(void)
{
	fw_memory_reset();
	fw_assert_reset();
	mock_io_reset();
	memset(&g_nvmeTask, 0, sizeof(g_nvmeTask));
	memset(&g_hostDmaStatus, 0, sizeof(g_hostDmaStatus));
	memset(&g_hostDmaAssistStatus, 0, sizeof(g_hostDmaAssistStatus));
}

void tearDown(void) {}

static void test_set_nvme_admin_queue_writes_admin_queue_register(void)
{
	NVME_ADMIN_QUEUE_SET_REG expected;

	set_nvme_admin_queue(1, 1, 1);

	expected.dword = 0;
	expected.sqValid = 1;
	expected.cqValid = 1;
	expected.cqIrqEn = 1;
	TEST_ASSERT_NOT_NULL(mock_io_last_write(NVME_ADMIN_QUEUE_SET_REG_ADDR));
	TEST_ASSERT_EQUAL_HEX32(expected.dword, mock_io_last_write(NVME_ADMIN_QUEUE_SET_REG_ADDR)->value);
}

static void test_check_nvme_cc_en_reads_status_register(void)
{
	NVME_STATUS_REG status;

	status.dword = 0;
	status.ccEn = 1;
	mock_io_set_reg(NVME_STATUS_REG_ADDR, status.dword);

	TEST_ASSERT_EQUAL_UINT(1, check_nvme_cc_en());
	TEST_ASSERT_EQUAL_UINT(1, mock_io_read_count(NVME_STATUS_REG_ADDR));
}

static void test_set_auto_tx_dma_pushes_dma_command_and_advances_tail(void)
{
	set_auto_tx_dma(3, 1, 0x10000000U, NVME_COMMAND_AUTO_COMPLETION_ON);

	TEST_ASSERT_EQUAL_UINT(1, g_hostDmaStatus.fifoTail.autoDmaTx);
	TEST_ASSERT_TRUE(mock_io_write_count(HOST_DMA_CMD_FIFO_REG_ADDR + 12) >= 1);
}

int main(void)
{
	UNITY_BEGIN();
	RUN_TEST(test_set_nvme_admin_queue_writes_admin_queue_register);
	RUN_TEST(test_check_nvme_cc_en_reads_status_register);
	RUN_TEST(test_set_auto_tx_dma_pushes_dma_command_and_advances_tail);
	return UNITY_END();
}
