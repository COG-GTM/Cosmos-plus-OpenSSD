/* Host stand-in for the Xilinx BSP xil_exception.h (HOST_TEST build only). */
#ifndef XIL_EXCEPTION_H
#define XIL_EXCEPTION_H

#include "xil_types.h"

#define XIL_EXCEPTION_FIQ 0x40U
#define XIL_EXCEPTION_IRQ 0x80U
#define XIL_EXCEPTION_ALL 0xC0U

#define XIL_EXCEPTION_ID_FIRST 0U
#define XIL_EXCEPTION_ID_RESET 0U
#define XIL_EXCEPTION_ID_UNDEFINED_INT 1U
#define XIL_EXCEPTION_ID_SWI_INT 2U
#define XIL_EXCEPTION_ID_PREFETCH_ABORT_INT 3U
#define XIL_EXCEPTION_ID_DATA_ABORT_INT 4U
#define XIL_EXCEPTION_ID_IRQ_INT 5U
#define XIL_EXCEPTION_ID_FIQ_INT 6U
#define XIL_EXCEPTION_ID_LAST 6U
#define XIL_EXCEPTION_ID_INT XIL_EXCEPTION_ID_IRQ_INT

typedef void (*Xil_ExceptionHandler)(void *data);

static inline void Xil_ExceptionInit(void) {}
static inline void Xil_ExceptionEnable(void) {}
static inline void Xil_ExceptionDisable(void) {}
static inline void Xil_ExceptionEnableMask(u32 Mask) { (void)Mask; }
static inline void Xil_ExceptionDisableMask(u32 Mask) { (void)Mask; }
static inline void Xil_ExceptionRegisterHandler(u32 Exception_id, Xil_ExceptionHandler Handler, void *Data)
{
	(void)Exception_id; (void)Handler; (void)Data;
}
static inline void Xil_ExceptionRemoveHandler(u32 Exception_id) { (void)Exception_id; }

#endif
