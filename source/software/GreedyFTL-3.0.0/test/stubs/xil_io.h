/* Host-side stub of xil_io.h: register access is redirected to the IO mock. */
#ifndef XIL_IO_H
#define XIL_IO_H
#include "xil_types.h"
#include "mock_io.h"
#define Xil_In32(addr)        mock_io_read32((UINTPTR)(addr))
#define Xil_Out32(addr, val)  mock_io_write32((UINTPTR)(addr), (u32)(val))
#define Xil_In16(addr)        ((u16)mock_io_read32((UINTPTR)(addr)))
#define Xil_Out16(addr, val)  mock_io_write32((UINTPTR)(addr), (u32)(val))
#define Xil_In8(addr)         ((u8)mock_io_read32((UINTPTR)(addr)))
#define Xil_Out8(addr, val)   mock_io_write32((UINTPTR)(addr), (u32)(val))
#endif
