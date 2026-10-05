#include "unity.h"

#include <string.h>

#include "fake_host_dma.h"
#include "host_reg_map.h"
#include "nvme/host_lld.h"
#include "nvme/nvme.h"
#include "nvme/nvme_identify.h"
#include "host_dram.h"
#include "ftl_config.h"

void setUp(void) {}
void tearDown(void) {}

void test_dma_register_writes_and_fifo_tail(void)
{
    const fake_host_dma_command *command;
    uint32_t value;
    host_reg_reset();
    fake_host_dma_install();

    set_direct_tx_dma(0x12345678, 2, 0x10203040, 0x800);
    TEST_ASSERT_EQUAL_UINT(1, g_hostDmaStatus.fifoTail.directDmaTx);
    TEST_ASSERT_EQUAL_UINT(1, fake_host_dma_log_count());
    command = fake_host_dma_log_get(0);
    TEST_ASSERT_NOT_NULL(command);
    TEST_ASSERT_EQUAL_UINT(0x12345678, command->words[0]);
    TEST_ASSERT_EQUAL_UINT(2, command->words[1]);
    TEST_ASSERT_EQUAL_UINT(0x10203040, command->words[2]);
    TEST_ASSERT_EQUAL_UINT(0x800, command->words[3] & 0x1fff);
    TEST_ASSERT_EQUAL_UINT(HOST_DMA_DIRECT_TYPE, command->type);
    TEST_ASSERT_EQUAL_UINT(HOST_DMA_TX_DIRECTION, command->direction);
    TEST_ASSERT_EQUAL_UINT(0x12345678, command->devAddr);
    TEST_ASSERT_EQUAL_UINT(0x800, command->len);

    set_auto_rx_dma(19, 7, 0x87654321, 1);
    TEST_ASSERT_EQUAL_UINT(1, g_hostDmaStatus.fifoTail.autoDmaRx);
    TEST_ASSERT_EQUAL_UINT(2, fake_host_dma_log_count());
    command = fake_host_dma_log_get(1);
    TEST_ASSERT_NOT_NULL(command);
    TEST_ASSERT_EQUAL_UINT(HOST_DMA_AUTO_TYPE, command->type);
    TEST_ASSERT_EQUAL_UINT(HOST_DMA_RX_DIRECTION, command->direction);
    TEST_ASSERT_EQUAL_UINT(19, command->cmdSlotTag);
    TEST_ASSERT_EQUAL_UINT(7, command->cmd4KBOffset);
    TEST_ASSERT_EQUAL_UINT(0x87654321, command->devAddr);
    TEST_ASSERT_TRUE(host_reg_last_write(HOST_DMA_CMD_FIFO_REG_ADDR, &value));
    TEST_ASSERT_EQUAL_UINT(0x87654321, value);
    TEST_ASSERT_TRUE(host_reg_last_write(HOST_DMA_CMD_FIFO_REG_ADDR + 12, &value));
    TEST_ASSERT_EQUAL_UINT(19U << 23 | 7U << 14 | 1U << 13, value);

    set_nvme_csts_rdy(1);
    TEST_ASSERT_TRUE(host_reg_last_write(NVME_STATUS_REG_ADDR, &value));
    TEST_ASSERT_EQUAL_UINT(1U << 4, value);
}

void test_identify_controller_strings_use_host_dram(void)
{
    ADMIN_IDENTIFY_CONTROLLER *identify =
        (ADMIN_IDENTIFY_CONTROLLER *)host_dram_resolve((void *)(uintptr_t)ADMIN_CMD_DRAM_DATA_BUFFER);
    identify_controller(ADMIN_CMD_DRAM_DATA_BUFFER);
    TEST_ASSERT_EQUAL_MEMORY(SERIAL_NUMBER, identify->SN, sizeof(SERIAL_NUMBER));
    TEST_ASSERT_EQUAL_MEMORY(MODEL_NUMBER, identify->MN, sizeof(MODEL_NUMBER));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_dma_register_writes_and_fifo_tail);
    RUN_TEST(test_identify_controller_strings_use_host_dram);
    return UNITY_END();
}
