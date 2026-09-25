/* Host-side stub of xtime_l.h backed by a deterministic, test-controlled counter. */
#ifndef XTIME_L_H
#define XTIME_L_H
#include "xil_types.h"
#define COUNTS_PER_SECOND (XPAR_CPU_CORTEXA9_0_CPU_CLK_FREQ_HZ / 2)
typedef u64 XTime;
void XTime_GetTime(XTime *Xtime);
void XTime_SetTime(XTime Xtime);
#endif
