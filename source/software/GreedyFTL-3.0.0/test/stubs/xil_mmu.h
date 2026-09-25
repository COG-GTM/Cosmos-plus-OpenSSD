#ifndef XIL_MMU_H
#define XIL_MMU_H
/* Host stub: no MMU control on the host. */
#include "xil_types.h"
static inline void Xil_SetTlbAttributes(INTPTR addr, u32 attrib) { (void)addr; (void)attrib; }
static inline void Xil_EnableMMU(void) {}
static inline void Xil_DisableMMU(void) {}
#endif
