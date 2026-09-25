#ifndef XIL_PRINTF_H
#define XIL_PRINTF_H
/* Host stub: xil_printf goes to stdout (silenced unless HOST_TEST_VERBOSE is set). */
#include <stdio.h>
void xil_printf(const char *fmt, ...);
/* Console input used by InitBlockDieMap(); returns 0 so the bad-block table is never re-made. */
char inbyte(void);
void outbyte(char c);
#endif
