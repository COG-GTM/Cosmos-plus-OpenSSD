/* Host stub for the Xilinx BSP xil_printf.h: output is captured by the test harness. */
#ifndef XIL_PRINTF_H
#define XIL_PRINTF_H

#include "xil_types.h"

void xil_printf(const char *format, ...);
void print(const char *str);
char inbyte(void);
void outbyte(char c);

#endif
