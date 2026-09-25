/*
 * Host memory arena standing in for the board's DRAM.
 *
 * The firmware carries every buffer address as an unsigned int, so the arena is mapped
 * below 4 GiB and its base is published through hostDramBaseAddr (see
 * stubs/host_memory_map.h). host_mem_init() is idempotent; host_mem_clear() zero-fills
 * the arena between tests without remapping it.
 */
#ifndef HOST_MEM_H
#define HOST_MEM_H

#include <stddef.h>

extern unsigned int hostDramBaseAddr;
extern unsigned int hostAdminCmdDataBufferAddr;

#define HOST_ADMIN_CMD_DATA_BUFFER_SIZE	0x00010000

void host_mem_init(void);
void host_mem_clear(void);
size_t host_mem_size(void);

static inline void *host_mem_ptr(unsigned int addr) { return (void *)(size_t)addr; }
static inline unsigned int host_mem_addr(const void *ptr) { return (unsigned int)(size_t)ptr; }

#endif
