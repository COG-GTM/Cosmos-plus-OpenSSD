/* Host-native stub of the Xilinx standalone BSP xil_mmu.h (no-ops). */
#ifndef XIL_MMU_H
#define XIL_MMU_H

#include "xil_types.h"

static inline void Xil_SetTlbAttributes(UINTPTR addr, u32 attrib) { (void)addr; (void)attrib; }

#endif /* XIL_MMU_H */
