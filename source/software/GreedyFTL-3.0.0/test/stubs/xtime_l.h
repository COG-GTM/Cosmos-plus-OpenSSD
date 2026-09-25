/* Host stand-in for the Xilinx BSP xtime_l.h (HOST_TEST build only).
 * XTime is backed by the host monotonic clock scaled to the Zynq global timer rate. */
#ifndef XTIME_L_H
#define XTIME_L_H

#include "xil_types.h"
#include "xparameters.h"

typedef u64 XTime;

#define COUNTS_PER_SECOND (XPAR_CPU_CORTEXA9_0_CPU_CLK_FREQ_HZ / 2)

void XTime_SetTime(XTime Xtime_Global);
void XTime_GetTime(XTime *Xtime_Global);

#endif
