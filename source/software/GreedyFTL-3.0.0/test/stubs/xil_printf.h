/*
 * Host-native stub of the Xilinx standalone BSP xil_printf.h.
 * Output is discarded unless GREEDYFTL_TEST_VERBOSE is set in the
 * environment; the last formatted line is kept for assertions.
 */
#ifndef XIL_PRINTF_H
#define XIL_PRINTF_H

#include "xil_types.h"

void xil_printf(const char8 *ctrl1, ...) __attribute__((format(printf, 1, 2)));
void print(const char8 *ptr);
void outbyte(char8 c);
char8 inbyte(void);

/* Test helpers. */
void stub_xil_printf_reset(void);
unsigned int stub_xil_printf_count(void);
const char *stub_xil_printf_last(void);
void stub_inbyte_set(char8 c);

#endif /* XIL_PRINTF_H */
