/*
 * HOST_TEST replacement for the IO_READ32/IO_WRITE32 macros of nvme/io_access.h.
 * Every NVMe host-controller register access is routed to the fake register map so
 * tests can pre-load register values and assert on register writes.
 */
#ifndef HOST_IO_ACCESS_H
#define HOST_IO_ACCESS_H

#include "fake_regs.h"

#define IO_WRITE32(addr, val)		fake_reg_write32((unsigned int)(addr), (unsigned int)(val))
#define IO_READ32(addr)				fake_reg_read32((unsigned int)(addr))

#endif
