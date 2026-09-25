#ifndef XIL_IO_H
#define XIL_IO_H
/* Host stub: register access routed to the in-memory fake register map. */
#include "xil_types.h"
#include "fake_regs.h"
static inline u32 Xil_In32(UINTPTR addr) { return fake_reg_read((unsigned int)addr); }
static inline void Xil_Out32(UINTPTR addr, u32 value) { fake_reg_write((unsigned int)addr, value); }
static inline u16 Xil_In16(UINTPTR addr) { return (u16)fake_reg_read((unsigned int)addr); }
static inline void Xil_Out16(UINTPTR addr, u16 value) { fake_reg_write((unsigned int)addr, value); }
static inline u8 Xil_In8(UINTPTR addr) { return (u8)fake_reg_read((unsigned int)addr); }
static inline void Xil_Out8(UINTPTR addr, u8 value) { fake_reg_write((unsigned int)addr, value); }
#endif
