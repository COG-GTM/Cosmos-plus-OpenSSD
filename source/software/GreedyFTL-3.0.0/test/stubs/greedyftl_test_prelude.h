/*
 * Force-included (-include) ahead of every firmware translation unit.
 *
 * - nvme/io_access.h: IO_READ32/IO_WRITE32 are redirected to mock_io so
 *   register accesses are captured instead of dereferencing MMIO addresses.
 * - nvme/debug.h: ASSERT() is redirected to fw_assert_fail() instead of
 *   spinning in while(1); the assert.h it pulled in is included here.
 *
 * xparameters.h is pre-included because host_lld.h relies on the Xilinx
 * BSP headers pulling it in transitively.
 *
 * Both headers are neutralised by pre-defining their include guards, so the
 * firmware sources are compiled unmodified.
 */
#ifndef GREEDYFTL_TEST_PRELUDE_H
#define GREEDYFTL_TEST_PRELUDE_H

#include "assert.h"
#include "xparameters.h"
#include "fw_assert.h"
#include "mock_io.h"

#define __IO_ACCESS_H_
#define IO_WRITE32(addr, val) mock_io_write32((unsigned int)(addr), (unsigned int)(val))
#define IO_READ32(addr) mock_io_read32((unsigned int)(addr))

#define __DEBUG_H_
#define __ASSERT 1
#define ASSERT(X)                                              \
	do {                                                       \
		if (!(X))                                              \
			fw_assert_fail(__FILE__, __LINE__, #X);            \
	} while (0)

#endif /* GREEDYFTL_TEST_PRELUDE_H */
