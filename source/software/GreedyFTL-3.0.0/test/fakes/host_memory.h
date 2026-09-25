#ifndef HOST_MEMORY_H_
#define HOST_MEMORY_H_

/*
 * Host replacement for the fixed Zynq DRAM layout in memory_map.h.
 *
 * The firmware stores every FTL table at an absolute 32-bit DRAM address and
 * passes those addresses around as `unsigned int`. On a 64-bit host we map one
 * anonymous arena below 4 GiB and expose its base through hostTestDramBase so
 * the same `unsigned int` arithmetic keeps working.
 */

#include <stddef.h>

#define HOST_TEST_RESERVED_DATA_BUFFER_SIZE  0x00200000u
#define HOST_TEST_TEMPORARY_PAY_LOAD_SIZE    0x00001000u
#define HOST_TEST_ADMIN_CMD_BUFFER_SIZE      0x00010000u

extern unsigned int hostTestDramBase;

#define HOST_TEST_DRAM_BASE_ADDR         (hostTestDramBase)
#define HOST_TEST_ADMIN_CMD_BUFFER_ADDR  (hostTestDramBase - HOST_TEST_ADMIN_CMD_BUFFER_SIZE)

/* Maps the arena (idempotent) and zero-fills it. Aborts if no <4 GiB mapping is available. */
void host_memory_init(void);
size_t host_memory_size(void);
void *host_memory_ptr(unsigned int addr);

#endif
