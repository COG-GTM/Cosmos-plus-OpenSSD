#ifndef XIL_EXCEPTION_H
#define XIL_EXCEPTION_H
/* Host stub: exception/IRQ control is a no-op. */
#include "xil_types.h"
#define XIL_EXCEPTION_ID_INT 5U
#define XIL_EXCEPTION_IRQ 0x80U
typedef void (*Xil_ExceptionHandler)(void *data);
static inline void Xil_ExceptionInit(void) {}
static inline void Xil_ExceptionEnable(void) {}
static inline void Xil_ExceptionDisable(void) {}
static inline void Xil_ExceptionEnableMask(u32 m) { (void)m; }
static inline void Xil_ExceptionDisableMask(u32 m) { (void)m; }
static inline void Xil_ExceptionRegisterHandler(u32 id, Xil_ExceptionHandler h, void *d) { (void)id; (void)h; (void)d; }
#endif
