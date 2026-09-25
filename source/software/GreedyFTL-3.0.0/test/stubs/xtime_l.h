#ifndef XTIME_L_H
#define XTIME_L_H

#include <time.h>
#include "xil_types.h"

typedef u64 XTime;

#define COUNTS_PER_SECOND 333333333ULL

static inline void XTime_GetTime(XTime *t)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	*t = (XTime)ts.tv_sec * COUNTS_PER_SECOND + (XTime)ts.tv_nsec / 3;
}

#endif
