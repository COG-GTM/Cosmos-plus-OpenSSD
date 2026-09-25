/* Force-included (-include) into every firmware translation unit built for the
 * host. It pre-empts two firmware headers that cannot be shadowed through the
 * include path because they are included with a quoted path relative to the
 * including file:
 *
 *   nvme/io_access.h  -> IO_READ32/IO_WRITE32 become calls into the IO mock
 *   nvme/debug.h      -> ASSERT() is routed to the harness assert hook
 *
 * Both headers are include-guarded, so defining their guard macros here makes
 * the real bodies compile to nothing. */
#ifndef FTL_TEST_PRELUDE_H
#define FTL_TEST_PRELUDE_H

#include <stdint.h>
#include "xparameters.h"
#include "mock_io.h"
#include "assert.h"

#define __IO_ACCESS_H_
#define IO_WRITE32(addr, val)  mock_io_write32((uintptr_t)(addr), (uint32_t)(val))
#define IO_READ32(addr)        mock_io_read32((uintptr_t)(addr))

#define __DEBUG_H_
#define __ASSERT 1
#define ASSERT(X) assert(X)

#endif
