/* Host-native stub of the Xilinx standalone BSP xil_cache.h (no-ops). */
#ifndef XIL_CACHE_H
#define XIL_CACHE_H

#include "xil_types.h"

static inline void Xil_DCacheEnable(void) {}
static inline void Xil_DCacheDisable(void) {}
static inline void Xil_ICacheEnable(void) {}
static inline void Xil_ICacheDisable(void) {}
static inline void Xil_DCacheFlush(void) {}
static inline void Xil_DCacheFlushRange(UINTPTR addr, u32 len) { (void)addr; (void)len; }
static inline void Xil_DCacheInvalidateRange(UINTPTR addr, u32 len) { (void)addr; (void)len; }

#endif /* XIL_CACHE_H */
