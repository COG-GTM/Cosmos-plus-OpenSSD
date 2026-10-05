#ifndef HOST_TEST_FAKE_HOST_DMA_H
#define HOST_TEST_FAKE_HOST_DMA_H
#include <stddef.h>
#include <stdint.h>
typedef struct {
    uint32_t words[4];
    uint32_t type;
    uint32_t direction;
    uint32_t devAddr;
    uint32_t cmdSlotTag;
    uint32_t cmd4KBOffset;
    uint32_t len;
} fake_host_dma_command;
void fake_host_dma_install(void);
void fake_host_dma_reset(void);
size_t fake_host_dma_log_count(void);
const fake_host_dma_command *fake_host_dma_log_get(size_t index);
#endif
