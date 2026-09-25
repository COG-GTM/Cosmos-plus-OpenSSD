#ifndef XIL_MMU_H
#define XIL_MMU_H
#include "xil_types.h"
#define Xil_SetTlbAttributes(addr, attr) ((void)(addr), (void)(attr))
#define Xil_EnableMMU()  ((void)0)
#define Xil_DisableMMU() ((void)0)
#endif
