/* Smoke test for nvme_admin_cmd.c + nvme_identify.c: Identify Controller
 * formats the controller data in DRAM and completes successfully. */
#include <string.h>
#include "unity.h"
#include "ftl_test_env.h"
#include "nvme/nvme.h"
#include "nvme/nvme_identify.h"
#include "nvme/nvme_admin_cmd.h"

void setUp(void) { ftl_test_env_reset(); }
void tearDown(void) {}

static void test_identify_controller_formats_response(void)
{
	NVME_ADMIN_COMMAND cmd;
	NVME_COMPLETION cpl;
	ADMIN_IDENTIFY_CONTROLLER *id = (ADMIN_IDENTIFY_CONTROLLER *)(uintptr_t)ADMIN_CMD_DRAM_DATA_BUFFER;

	memset(&cmd, 0, sizeof(cmd));
	memset(&cpl, 0xFF, sizeof(cpl));
	cmd.OPC = ADMIN_IDENTIFY;
	cmd.PRP1[0] = 0x10000000;
	cmd.dword10 = 1; /* CNS = controller */

	handle_identify(&cmd, &cpl);

	TEST_ASSERT_EQUAL_HEX16(PCI_VENDOR_ID, id->VID);
	TEST_ASSERT_EQUAL_MEMORY(SERIAL_NUMBER, id->SN, sizeof(SERIAL_NUMBER) - 1);
	TEST_ASSERT_EQUAL_UINT32(0, cpl.dword[0]);
	TEST_ASSERT_EQUAL_UINT(0, cpl.statusField.SC);
	TEST_ASSERT_TRUE(mock_io_write_count() > 0); /* DMA descriptors pushed */
}

int main(void)
{
	UNITY_BEGIN();
	RUN_TEST(test_identify_controller_formats_response);
	return UNITY_END();
}
