/* Implementations for the Xilinx BSP stubs in test/stubs. */
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include "xil_printf.h"
#include "xtime_l.h"
#include "ftl_test_env.h"

static int quiet = -1;
static XTime fake_time;
static char inbyte_queue[64];
static size_t inbyte_head, inbyte_len;

void ftl_test_queue_inbyte(const char *bytes)
{
	inbyte_head = 0;
	inbyte_len = bytes ? strlen(bytes) : 0;
	if (inbyte_len > sizeof(inbyte_queue))
		inbyte_len = sizeof(inbyte_queue);
	if (inbyte_len)
		memcpy(inbyte_queue, bytes, inbyte_len);
}

char inbyte(void)
{
	if (inbyte_head < inbyte_len)
		return inbyte_queue[inbyte_head++];
	return '\n';
}

static int is_quiet(void)
{
	if (quiet < 0)
	{
		const char *v = getenv("FTL_TEST_VERBOSE");
		quiet = (v == NULL || *v == '\0' || strcmp(v, "0") == 0);
	}
	return quiet;
}

void ftl_test_set_verbose(int verbose)
{
	quiet = !verbose;
}

void xil_printf(const char *fmt, ...)
{
	va_list ap;
	ftl_test_printf_count++;
	if (is_quiet())
		return;
	va_start(ap, fmt);
	vprintf(fmt, ap);
	va_end(ap);
}

void print(const char *s)
{
	ftl_test_printf_count++;
	if (!is_quiet())
		fputs(s, stdout);
}

void outbyte(char c)
{
	if (!is_quiet())
		putchar(c);
}

void XTime_GetTime(XTime *t)
{
	fake_time += 1000;
	if (t)
		*t = fake_time;
}

void XTime_SetTime(XTime t)
{
	fake_time = t;
}
