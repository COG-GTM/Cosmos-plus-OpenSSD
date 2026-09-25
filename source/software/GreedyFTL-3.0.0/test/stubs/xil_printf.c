#include "xil_printf.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

static unsigned int printf_count;
static char last_line[512];
static char8 next_inbyte = '\r';

static int verbose(void)
{
	static int cached = -1;

	if (cached < 0)
		cached = getenv("GREEDYFTL_TEST_VERBOSE") != NULL;
	return cached;
}

void xil_printf(const char8 *ctrl1, ...)
{
	va_list args;

	va_start(args, ctrl1);
	vsnprintf(last_line, sizeof(last_line), ctrl1, args);
	va_end(args);
	printf_count++;
	if (verbose())
		fputs(last_line, stdout);
}

void print(const char8 *ptr) { xil_printf("%s", ptr); }
void outbyte(char8 c) { xil_printf("%c", c); }
char8 inbyte(void) { return next_inbyte; }

void stub_xil_printf_reset(void)
{
	printf_count = 0;
	last_line[0] = '\0';
	next_inbyte = '\r';
}

unsigned int stub_xil_printf_count(void) { return printf_count; }
const char *stub_xil_printf_last(void) { return last_line; }
void stub_inbyte_set(char8 c) { next_inbyte = c; }
