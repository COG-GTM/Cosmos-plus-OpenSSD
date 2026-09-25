/* Host-side stub of the Xilinx BSP xil_printf.h.
 * Output is routed through the test harness so tests can silence or capture it. */
#ifndef XIL_PRINTF_H
#define XIL_PRINTF_H
#include <stdarg.h>
#include "xil_types.h"
#ifdef __cplusplus
extern "C" {
#endif
void xil_printf(const char *fmt, ...);
void print(const char *s);
void outbyte(char c);
char inbyte(void);
#ifdef __cplusplus
}
#endif
#endif
