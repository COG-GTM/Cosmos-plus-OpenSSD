#include "fw_test.h"

#include <string.h>

#include "address_translation.h"
#include "ftl_config.h"
#include "memory_map.h"
#include "nvme/nvme.h"
#include "xil_printf.h"

extern volatile NVME_CONTEXT g_nvmeTask;

/*
 * FindDieForFreeSliceAllocation() keeps its round-robin (channel, way) cursor
 * in function-local statics that survive fw_memory_reset(). Advance it until
 * it has handed out the last die of the cycle so the next call returns die 0.
 */
static void rewind_die_allocator(void)
{
	const unsigned int lastDie = Pcw2VdieTranslation(USER_CHANNELS - 1, USER_WAYS - 1);
	unsigned int i;

	for (i = 0; i < USER_DIES; i++)
		if (FindDieForFreeSliceAllocation() == lastDie)
			return;
}

void fw_test_reset(void)
{
	fw_memory_reset();
	fw_assert_reset();
	mock_io_reset();
	mock_nsc_reset();
	mock_host_reset();
	stub_xil_printf_reset();
	memset((void *)&g_nvmeTask, 0, sizeof(g_nvmeTask));
	rewind_die_allocator();
}

void fw_test_init_ftl(void)
{
	InitFTL();
	mock_nsc_clear_calls();
}
