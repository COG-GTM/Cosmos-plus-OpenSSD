#ifndef HOST_TEST_REG_MAP_H
#define HOST_TEST_REG_MAP_H
#include <stddef.h>
#include <stdint.h>
typedef uint32_t (*host_reg_read_hook)(uint32_t address, void *context);
typedef struct {
    uint32_t address;
    uint32_t value;
    unsigned long sequence;
} host_reg_write;
void host_reg_write32(uint32_t address, uint32_t value);
uint32_t host_reg_read32(uint32_t address);
void host_reg_reset(void);
void host_reg_set_read_hook(uint32_t address, host_reg_read_hook hook, void *context);
size_t host_reg_log_count(void);
const host_reg_write *host_reg_log_get(size_t index);
int host_reg_last_write(uint32_t address, uint32_t *value);
#endif
