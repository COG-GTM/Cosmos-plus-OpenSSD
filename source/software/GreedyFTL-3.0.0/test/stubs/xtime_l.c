#include "xtime_l.h"

static XTime now;

void XTime_GetTime(XTime *xtime) { *xtime = now; }
void XTime_SetTime(XTime xtime) { now = xtime; }
