/* Host stub for the Xilinx BSP xil_mmu.h */
#ifndef XIL_MMU_H
#define XIL_MMU_H

#include "xil_types.h"

static inline void Xil_SetTlbAttributes(INTPTR Addr, u32 attrib) { (void)Addr; (void)attrib; }
static inline void Xil_EnableMMU(void) {}
static inline void Xil_DisableMMU(void) {}

#endif
