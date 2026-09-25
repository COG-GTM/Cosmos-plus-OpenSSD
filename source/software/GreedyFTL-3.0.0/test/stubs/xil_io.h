// Host-test stand-in for xil_io.h: register accesses go to an in-memory fake register map.
#ifndef XIL_IO_H
#define XIL_IO_H

#include "xil_types.h"
#include "fake_regs.h"

#define Xil_In32(addr)			fake_reg_read32((UINTPTR)(addr))
#define Xil_Out32(addr, val)	fake_reg_write32((UINTPTR)(addr), (u32)(val))

#endif
