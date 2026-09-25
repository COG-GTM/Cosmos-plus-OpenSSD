/* Unit tests for nvme/nvme_admin_cmd.c and nvme/nvme_identify.c. */
#include "unity.h"

#include <string.h>

#include "fw_test.h"
#include "nvme/nvme.h"
#include "nvme/nvme_admin_cmd.h"
#include "nvme/nvme_identify.h"

void setUp(void) { fw_test_reset(); }
void tearDown(void) {}

static NVME_COMMAND make_admin_cmd(unsigned char opc, unsigned short cmdSlotTag)
{
	NVME_COMMAND cmd;
	NVME_ADMIN_COMMAND *admin = (NVME_ADMIN_COMMAND *)cmd.cmdDword;

	memset(&cmd, 0, sizeof(cmd));
	cmd.cmdSlotTag = cmdSlotTag;
	admin->OPC = opc;
	return cmd;
}

static void test_smoke_identify_controller_fills_buffer_and_completes(void)
{
	NVME_COMMAND cmd = make_admin_cmd(ADMIN_IDENTIFY, 3);
	NVME_ADMIN_COMMAND *admin = (NVME_ADMIN_COMMAND *)cmd.cmdDword;
	ADMIN_IDENTIFY_CONTROLLER *identify = fw_ptr(ADMIN_CMD_DRAM_DATA_BUFFER);

	admin->dword10 = 1; /* CNS = controller */
	admin->PRP1[0] = 0x1000;

	handle_nvme_admin_cmd(&cmd);

	TEST_ASSERT_EQUAL_HEX16(PCI_VENDOR_ID, identify->VID);
	TEST_ASSERT_EQUAL_UINT(1, mock_host_count(MOCK_HOST_SET_DIRECT_TX_DMA));
	TEST_ASSERT_EQUAL_UINT(1, mock_host_count(MOCK_HOST_SET_AUTO_NVME_CPL));
	TEST_ASSERT_EQUAL_UINT(3, mock_host_last(MOCK_HOST_SET_AUTO_NVME_CPL)->args[0]);
}

int main(void)
{
	UNITY_BEGIN();
	RUN_TEST(test_smoke_identify_controller_fills_buffer_and_completes);
	return UNITY_END();
}
