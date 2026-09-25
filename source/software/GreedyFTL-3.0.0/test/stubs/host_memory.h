/* Host-side backing store for the firmware's fixed DRAM layout (HOST_TEST build only).
 *
 * memory_map.h places every FTL table at FTL_MANAGEMENT_START_ADDR + offset. On the
 * host, that base is `hostDramBase`, an mmap'd region placed below 4 GiB so the
 * firmware's `unsigned int` address arithmetic and pointer casts stay valid. */
#ifndef HOST_MEMORY_H
#define HOST_MEMORY_H

#ifdef __cplusplus
extern "C" {
#endif

extern unsigned int hostDramBase;

/* Maps the region (idempotent). Aborts with a message if no sub-4 GiB address is free. */
void HostMemoryInit(void);

/* Returns pages of the region to the kernel so the next test starts from zeroed memory. */
void HostMemoryClear(void);

unsigned int HostMemorySize(void);

#ifdef __cplusplus
}
#endif

#endif
