#include "fw_test.h"

#include <string.h>

#include "ftl_config.h"
#include "memory_map.h"
#include "nvme/nvme.h"
#include "xil_printf.h"

extern volatile NVME_CONTEXT g_nvmeTask;

void fw_test_reset(void)
{
	fw_memory_reset();
	fw_assert_reset();
	mock_io_reset();
	mock_nsc_reset();
	mock_host_reset();
	stub_xil_printf_reset();
	memset((void *)&g_nvmeTask, 0, sizeof(g_nvmeTask));
}

void fw_test_init_ftl(void)
{
	InitFTL();
}
