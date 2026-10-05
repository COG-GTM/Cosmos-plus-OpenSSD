#ifndef HOST_TEST_MEMORY_MAP_LIMITS_H
#define HOST_TEST_MEMORY_MAP_LIMITS_H

#include "xparameters.h"
#include "memory_map.h"

#undef DRAM_END_ADDR
#define DRAM_END_ADDR (HOST_DRAM_BASE + 0x3fffffffU)

#endif
