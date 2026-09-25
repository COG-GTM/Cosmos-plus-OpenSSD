#ifndef XTIME_L_H
#define XTIME_L_H
/* Host stub: global timer replaced by clock_gettime. */
#include "xil_types.h"
#include <time.h>
typedef u64 XTime;
#define COUNTS_PER_SECOND 333333343ULL
static inline void XTime_GetTime(XTime *t)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	*t = (XTime)ts.tv_sec * COUNTS_PER_SECOND + (XTime)ts.tv_nsec * COUNTS_PER_SECOND / 1000000000ULL;
}
static inline void XTime_SetTime(XTime t) { (void)t; }
#endif
