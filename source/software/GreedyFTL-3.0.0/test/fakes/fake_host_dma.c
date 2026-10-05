#include "fake_host_dma.h"

#include <string.h>

#include "host_reg_map.h"
#include "nvme/host_lld.h"

#define DMA_LOG_CAPACITY 4096

static fake_host_dma_command dma_commands[DMA_LOG_CAPACITY];
static size_t dma_command_count;

static uint32_t fifo_tail_read(uint32_t address, void *context)
{
    (void)address;
    (void)context;
    return g_hostDmaStatus.fifoTail.dword;
}

void fake_host_dma_install(void)
{
    fake_host_dma_reset();
    host_reg_set_read_hook(HOST_DMA_FIFO_CNT_REG_ADDR, fifo_tail_read, NULL);
}

void fake_host_dma_reset(void)
{
    dma_command_count = 0;
    memset(dma_commands, 0, sizeof(dma_commands));
    memset(&g_hostDmaStatus, 0, sizeof(g_hostDmaStatus));
    memset(&g_hostDmaAssistStatus, 0, sizeof(g_hostDmaAssistStatus));
}

size_t fake_host_dma_log_count(void)
{
    size_t i;
    size_t count = 0;
    for (i = 0; i + 1 < host_reg_log_count(); ++i) {
        const host_reg_write *a = host_reg_log_get(i);
        const host_reg_write *b = host_reg_log_get(i + 1);
        fake_host_dma_command *command;
        int direct = 0;
        if (i + 3 < host_reg_log_count()) {
            const host_reg_write *c = host_reg_log_get(i + 2);
            const host_reg_write *d = host_reg_log_get(i + 3);
            direct = a->address == HOST_DMA_CMD_FIFO_REG_ADDR &&
                     b->address == HOST_DMA_CMD_FIFO_REG_ADDR + 4 &&
                     c->address == HOST_DMA_CMD_FIFO_REG_ADDR + 8 &&
                     d->address == HOST_DMA_CMD_FIFO_REG_ADDR + 12;
        }
        if (direct) {
            const host_reg_write *c = host_reg_log_get(i + 2);
            const host_reg_write *d = host_reg_log_get(i + 3);
            command = count < DMA_LOG_CAPACITY ? &dma_commands[count] : NULL;
            if (command != NULL) {
                command->words[0] = a->value;
                command->words[1] = b->value;
                command->words[2] = c->value;
                command->words[3] = d->value;
            }
            i += 3;
        } else if (a->address == HOST_DMA_CMD_FIFO_REG_ADDR &&
                   b->address == HOST_DMA_CMD_FIFO_REG_ADDR + 12) {
            command = count < DMA_LOG_CAPACITY ? &dma_commands[count] : NULL;
            if (command != NULL) {
                command->words[0] = a->value;
                command->words[1] = 0;
                command->words[2] = 0;
                command->words[3] = b->value;
            }
        } else {
            continue;
        }
        if (command != NULL) {
            command->type = (command->words[3] >> 31) & 1U;
            command->direction = (command->words[3] >> 30) & 1U;
            command->cmdSlotTag = (command->words[3] >> 23) & 0x7fU;
            command->cmd4KBOffset = (command->words[3] >> 14) & 0x1ffU;
            command->len = command->words[3] & 0x1fffU;
            command->devAddr = command->words[0];
        }
        ++count;
    }
    dma_command_count = count < DMA_LOG_CAPACITY ? count : DMA_LOG_CAPACITY;
    return dma_command_count;
}

const fake_host_dma_command *fake_host_dma_log_get(size_t index)
{
    (void)fake_host_dma_log_count();
    return index < dma_command_count ? &dma_commands[index] : NULL;
}
