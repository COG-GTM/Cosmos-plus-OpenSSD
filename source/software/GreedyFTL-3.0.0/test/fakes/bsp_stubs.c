#include "xil_io.h"
#include "xil_printf.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

#include "host_reg_map.h"

static char input_byte = 'n';

int xil_printf(const char *format, ...)
{
    int result = 0;
    const char *verbose = getenv("HOST_TEST_VERBOSE");
    if (verbose != NULL && verbose[0] == '1') {
        va_list args;
        va_start(args, format);
        result = vprintf(format, args);
        va_end(args);
    }
    return result;
}

char inbyte(void)
{
    return input_byte;
}

void host_test_set_inbyte(char value)
{
    input_byte = value;
}

uint32_t Xil_In32(uintptr_t address)
{
    return host_reg_read32((uint32_t)address);
}

void Xil_Out32(uintptr_t address, uint32_t value)
{
    host_reg_write32((uint32_t)address, value);
}

uint16_t Xil_In16(uintptr_t address)
{
    uint32_t value = host_reg_read32((uint32_t)(address & ~(uintptr_t)3));
    return (uint16_t)(value >> ((address & 2) * 8));
}

void Xil_Out16(uintptr_t address, uint16_t value)
{
    uint32_t base = (uint32_t)(address & ~(uintptr_t)3);
    uint32_t old = host_reg_read32(base);
    unsigned int shift = (unsigned int)(address & 2) * 8;
    host_reg_write32(base, (old & ~(0xffffU << shift)) | ((uint32_t)value << shift));
}

uint8_t Xil_In8(uintptr_t address)
{
    uint32_t value = host_reg_read32((uint32_t)(address & ~(uintptr_t)3));
    return (uint8_t)(value >> ((address & 3) * 8));
}

void Xil_Out8(uintptr_t address, uint8_t value)
{
    uint32_t base = (uint32_t)(address & ~(uintptr_t)3);
    uint32_t old = host_reg_read32(base);
    unsigned int shift = (unsigned int)(address & 3) * 8;
    host_reg_write32(base, (old & ~(0xffU << shift)) | ((uint32_t)value << shift));
}
