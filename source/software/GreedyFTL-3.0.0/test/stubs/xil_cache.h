#ifndef XIL_CACHE_H
#define XIL_CACHE_H
/* Host stub: caches are coherent on the host, all maintenance ops are no-ops. */
#include "xil_types.h"
static inline void Xil_DCacheEnable(void) {}
static inline void Xil_DCacheDisable(void) {}
static inline void Xil_ICacheEnable(void) {}
static inline void Xil_ICacheDisable(void) {}
static inline void Xil_DCacheFlush(void) {}
static inline void Xil_DCacheInvalidate(void) {}
static inline void Xil_DCacheFlushRange(INTPTR a, u32 l) { (void)a; (void)l; }
static inline void Xil_DCacheInvalidateRange(INTPTR a, u32 l) { (void)a; (void)l; }
#endif
