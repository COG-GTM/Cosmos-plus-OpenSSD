// In-memory fake register map used by the host build in place of AXI MMIO.
// IO_READ32/IO_WRITE32 (nvme/io_access.h) and Xil_In32/Xil_Out32 route here.
#ifndef FAKE_REGS_H
#define FAKE_REGS_H

#include <stdint.h>

void fake_regs_reset(void);
uint32_t fake_reg_read32(uintptr_t addr);
void fake_reg_write32(uintptr_t addr, uint32_t val);
unsigned int fake_reg_write_count(void);

#ifdef IO_READ32
#undef IO_READ32
#endif
#ifdef IO_WRITE32
#undef IO_WRITE32
#endif
#define IO_READ32(addr)			fake_reg_read32((uintptr_t)(addr))
#define IO_WRITE32(addr, val)	fake_reg_write32((uintptr_t)(addr), (uint32_t)(val))

#endif
