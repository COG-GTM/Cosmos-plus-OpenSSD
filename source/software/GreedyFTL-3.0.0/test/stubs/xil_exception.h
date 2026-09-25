/* Host stub for the Xilinx BSP xil_exception.h */
#ifndef XIL_EXCEPTION_H
#define XIL_EXCEPTION_H

#include "xil_types.h"
/* The SDK BSP makes the XPAR_* peripheral map visible through this chain of headers. */
#include "xparameters.h"

#define XIL_EXCEPTION_ID_FIRST 0U
#define XIL_EXCEPTION_ID_INT 5U
#define XIL_EXCEPTION_ID_LAST 6U
#define XIL_EXCEPTION_IRQ 0x80U
#define XIL_EXCEPTION_FIQ 0x40U
#define XIL_EXCEPTION_ALL 0xC0U

typedef void (*Xil_ExceptionHandler)(void *data);

static inline void Xil_ExceptionInit(void) {}
static inline void Xil_ExceptionEnable(void) {}
static inline void Xil_ExceptionDisable(void) {}
static inline void Xil_ExceptionEnableMask(u32 mask) { (void)mask; }
static inline void Xil_ExceptionDisableMask(u32 mask) { (void)mask; }
static inline void Xil_ExceptionRegisterHandler(u32 id, Xil_ExceptionHandler handler, void *data)
{
	(void)id; (void)handler; (void)data;
}
static inline void Xil_ExceptionRemoveHandler(u32 id) { (void)id; }

#endif
