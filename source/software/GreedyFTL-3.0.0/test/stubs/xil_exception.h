#ifndef XIL_EXCEPTION_H
#define XIL_EXCEPTION_H
#include "xil_types.h"
typedef void (*Xil_ExceptionHandler)(void *data);
#define XIL_EXCEPTION_ID_INT 5U
#define XIL_EXCEPTION_IRQ    0x80U
#define Xil_ExceptionInit()                         ((void)0)
#define Xil_ExceptionEnable()                       ((void)0)
#define Xil_ExceptionDisable()                      ((void)0)
#define Xil_ExceptionEnableMask(m)                  ((void)(m))
#define Xil_ExceptionRegisterHandler(id, h, d)      ((void)(id), (void)(h), (void)(d))
#endif
