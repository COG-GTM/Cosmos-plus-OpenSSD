#ifndef XIL_PRINTF_H
#define XIL_PRINTF_H

#include <stdio.h>
#include "xil_types.h"

#define xil_printf(...) printf(__VA_ARGS__)
#define print(s) fputs((s), stdout)

#endif
