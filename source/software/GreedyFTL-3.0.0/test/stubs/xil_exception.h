/* Host-native stub of the Xilinx standalone BSP xil_exception.h (no-ops). */
#ifndef XIL_EXCEPTION_H
#define XIL_EXCEPTION_H

#include "xil_types.h"

#define XIL_EXCEPTION_ID_INT 5U

typedef void (*Xil_ExceptionHandler)(void *data);

static inline void Xil_ExceptionInit(void) {}
static inline void Xil_ExceptionEnable(void) {}
static inline void Xil_ExceptionDisable(void) {}
static inline void Xil_ExceptionRegisterHandler(u32 id, Xil_ExceptionHandler handler, void *data)
{
	(void)id;
	(void)handler;
	(void)data;
}

#endif /* XIL_EXCEPTION_H */
