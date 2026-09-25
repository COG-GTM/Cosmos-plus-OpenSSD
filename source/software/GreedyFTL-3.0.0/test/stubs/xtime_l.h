/* Host-native stub of the Xilinx standalone BSP xtime_l.h, driven by the test clock. */
#ifndef XTIME_L_H
#define XTIME_L_H

#include "xil_types.h"

typedef u64 XTime;

#define COUNTS_PER_SECOND 325000000ULL

void XTime_GetTime(XTime *xtime);
void XTime_SetTime(XTime xtime);

#endif /* XTIME_L_H */
