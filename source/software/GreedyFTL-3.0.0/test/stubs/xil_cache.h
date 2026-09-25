#ifndef XIL_CACHE_H
#define XIL_CACHE_H
#include "xil_types.h"
#define Xil_ICacheEnable()            ((void)0)
#define Xil_ICacheDisable()           ((void)0)
#define Xil_DCacheEnable()            ((void)0)
#define Xil_DCacheDisable()           ((void)0)
#define Xil_DCacheFlush()             ((void)0)
#define Xil_DCacheFlushRange(a, l)    ((void)(a), (void)(l))
#define Xil_DCacheInvalidateRange(a,l) ((void)(a), (void)(l))
#endif
