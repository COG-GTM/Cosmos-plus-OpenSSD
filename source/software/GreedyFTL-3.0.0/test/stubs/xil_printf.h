/* Host stand-in for the Xilinx BSP xil_printf.h (HOST_TEST build only).
 * Output is discarded unless the FTL_TEST_VERBOSE environment variable is set. */
#ifndef XIL_PRINTF_H
#define XIL_PRINTF_H

#include "xil_types.h"
#include "xparameters.h"

#ifdef __cplusplus
extern "C" {
#endif

void xil_printf(const char *format, ...);
void print(const char *str);
void outbyte(char c);
char inbyte(void);

/* Test hooks: byte returned by inbyte() (default 0, i.e. not 'X'). */
void XilStubSetInbyte(char c);

#ifdef __cplusplus
}
#endif

#endif
