#ifndef HOST_TEST_DRAM_H
#define HOST_TEST_DRAM_H
#include <stdint.h>
extern uintptr_t host_dram_base;
#define HOST_DRAM_BASE (host_dram_base)
void *host_dram_resolve(const void *address);
uintptr_t host_dram_offset(const void *address);
#endif
