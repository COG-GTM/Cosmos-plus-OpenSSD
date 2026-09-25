/* Host stand-in for the Xilinx BSP xil_io.h (HOST_TEST build only).
 * All accesses are routed to the in-memory fake register map. */
#ifndef XIL_IO_H
#define XIL_IO_H

#include "xil_types.h"
#include "fake_regs.h"

static inline u32 Xil_In32(UINTPTR Addr) { return FakeRegRead32((unsigned int)Addr); }
static inline void Xil_Out32(UINTPTR Addr, u32 Value) { FakeRegWrite32((unsigned int)Addr, Value); }

static inline u16 Xil_In16(UINTPTR Addr) { return (u16)FakeRegRead32((unsigned int)Addr); }
static inline void Xil_Out16(UINTPTR Addr, u16 Value) { FakeRegWrite32((unsigned int)Addr, Value); }
static inline u8 Xil_In8(UINTPTR Addr) { return (u8)FakeRegRead32((unsigned int)Addr); }
static inline void Xil_Out8(UINTPTR Addr, u8 Value) { FakeRegWrite32((unsigned int)Addr, Value); }

#endif
