#include "host_reg_map.h"

#include <string.h>

#define REG_CAPACITY 4096
#define LOG_CAPACITY 32768
#define HOOK_CAPACITY 32

typedef struct {
    uint32_t address;
    uint32_t value;
    int used;
} register_entry;

typedef struct {
    uint32_t address;
    host_reg_read_hook hook;
    void *context;
} read_hook_entry;

static register_entry registers[REG_CAPACITY];
static host_reg_write writes[LOG_CAPACITY];
static read_hook_entry hooks[HOOK_CAPACITY];
static size_t write_count;
static size_t hook_count;

static register_entry *find_register(uint32_t address, int create)
{
    size_t i;
    for (i = 0; i < REG_CAPACITY; ++i) {
        if (registers[i].used && registers[i].address == address)
            return &registers[i];
    }
    if (!create)
        return NULL;
    for (i = 0; i < REG_CAPACITY; ++i) {
        if (!registers[i].used) {
            registers[i].used = 1;
            registers[i].address = address;
            registers[i].value = 0;
            return &registers[i];
        }
    }
    return NULL;
}

void host_reg_write32(uint32_t address, uint32_t value)
{
    register_entry *entry = find_register(address, 1);
    if (entry != NULL)
        entry->value = value;
    if (write_count < LOG_CAPACITY) {
        writes[write_count].address = address;
        writes[write_count].value = value;
        writes[write_count].sequence = (unsigned long)write_count;
        ++write_count;
    }
}

uint32_t host_reg_read32(uint32_t address)
{
    size_t i;
    for (i = 0; i < hook_count; ++i) {
        if (hooks[i].address == address)
            return hooks[i].hook(address, hooks[i].context);
    }
    {
        register_entry *entry = find_register(address, 0);
        return entry == NULL ? 0 : entry->value;
    }
}

void host_reg_set_read_hook(uint32_t address, host_reg_read_hook hook, void *context)
{
    size_t i;
    for (i = 0; i < hook_count; ++i) {
        if (hooks[i].address == address) {
            hooks[i].hook = hook;
            hooks[i].context = context;
            return;
        }
    }
    if (hook_count < HOOK_CAPACITY) {
        hooks[hook_count].address = address;
        hooks[hook_count].hook = hook;
        hooks[hook_count].context = context;
        ++hook_count;
    }
}

void host_reg_reset(void)
{
    memset(registers, 0, sizeof(registers));
    memset(writes, 0, sizeof(writes));
    memset(hooks, 0, sizeof(hooks));
    write_count = 0;
    hook_count = 0;
}

size_t host_reg_log_count(void)
{
    return write_count;
}

const host_reg_write *host_reg_log_get(size_t index)
{
    return index < write_count ? &writes[index] : NULL;
}

int host_reg_last_write(uint32_t address, uint32_t *value)
{
    size_t i = write_count;
    while (i > 0) {
        --i;
        if (writes[i].address == address) {
            if (value != NULL)
                *value = writes[i].value;
            return 1;
        }
    }
    return 0;
}
