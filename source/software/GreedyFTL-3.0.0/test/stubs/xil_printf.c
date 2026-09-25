#include "xil_printf.h"
#include <stdarg.h>
#include <stdlib.h>

void xil_printf(const char *fmt, ...)
{
	va_list ap;
	if (getenv("HOST_TEST_VERBOSE") == NULL)
		return;
	va_start(ap, fmt);
	vprintf(fmt, ap);
	va_end(ap);
}

char inbyte(void)
{
	return 0;
}

void outbyte(char c)
{
	if (getenv("HOST_TEST_VERBOSE") != NULL)
		putchar(c);
}
