/* Host stand-in for the Xilinx BSP xil_cache.h (HOST_TEST build only).
 * Cache maintenance is a no-op on the host. */
#ifndef XIL_CACHE_H
#define XIL_CACHE_H

#include "xil_types.h"

static inline void Xil_DCacheEnable(void) {}
static inline void Xil_DCacheDisable(void) {}
static inline void Xil_DCacheInvalidate(void) {}
static inline void Xil_DCacheInvalidateRange(INTPTR adr, u32 len) { (void)adr; (void)len; }
static inline void Xil_DCacheFlush(void) {}
static inline void Xil_DCacheFlushRange(INTPTR adr, u32 len) { (void)adr; (void)len; }
static inline void Xil_ICacheEnable(void) {}
static inline void Xil_ICacheDisable(void) {}
static inline void Xil_ICacheInvalidate(void) {}
static inline void Xil_ICacheInvalidateRange(INTPTR adr, u32 len) { (void)adr; (void)len; }

#endif
