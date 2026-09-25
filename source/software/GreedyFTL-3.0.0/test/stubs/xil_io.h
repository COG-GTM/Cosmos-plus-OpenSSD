/* Host-native stub of the Xilinx standalone BSP xil_io.h, routed to mock_io. */
#ifndef XIL_IO_H
#define XIL_IO_H

#include "xil_types.h"
#include "mock_io.h"

static inline u32 Xil_In32(UINTPTR addr) { return mock_io_read32((unsigned int)addr); }
static inline void Xil_Out32(UINTPTR addr, u32 value) { mock_io_write32((unsigned int)addr, value); }

#endif /* XIL_IO_H */
