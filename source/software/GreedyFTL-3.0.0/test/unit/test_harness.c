/* Proves the host harness itself: emulated DRAM window, IO mock, NSC mock,
 * assert capture, and a full InitFTL() boot against the mocked NAND array. */
#include "unity.h"
#include "ftl_test_env.h"
#include "memory_map.h"
#include "nvme/host_lld.h"

void setUp(void)
{
	ftl_test_env_reset();
}

void tearDown(void)
{
}

static void test_dram_window_is_mapped_and_zeroed(void)
{
	volatile unsigned int *lo = (volatile unsigned int *)(uintptr_t)FTL_TEST_DRAM_BASE;
	volatile unsigned int *hi = (volatile unsigned int *)(uintptr_t)(FTL_TEST_DRAM_END - 4);
	TEST_ASSERT_EQUAL_UINT32(0, *lo);
	TEST_ASSERT_EQUAL_UINT32(0, *hi);
	*lo = 0xA5A5A5A5u;
	*hi = 0x5A5A5A5Au;
	TEST_ASSERT_EQUAL_HEX32(0xA5A5A5A5u, *lo);
	TEST_ASSERT_EQUAL_HEX32(0x5A5A5A5Au, *hi);
	ftl_test_env_reset();
	TEST_ASSERT_EQUAL_UINT32(0, *lo);
}

static void test_memory_map_fits_in_dram_window(void)
{
	TEST_ASSERT_TRUE(FTL_MANAGEMENT_END_ADDR < FTL_TEST_DRAM_END);
	TEST_ASSERT_TRUE(RESERVED_DATA_BUFFER_BASE_ADDR + 0x00200000 <= COMPLETE_FLAG_TABLE_ADDR);
	TEST_ASSERT_TRUE(TEMPORARY_PAY_LOAD_ADDR + 0x00001000 <= DATA_BUFFER_MAP_ADDR);
}

static void test_io_mock_records_writes_and_serves_reads(void)
{
	int found = 0;
	IO_WRITE32(NVME_STATUS_REG_ADDR, 0x12345678u);
	TEST_ASSERT_EQUAL_HEX32(0x12345678u, IO_READ32(NVME_STATUS_REG_ADDR));
	TEST_ASSERT_EQUAL_size_t(1, mock_io_write_count_for(NVME_STATUS_REG_ADDR));
	TEST_ASSERT_EQUAL_HEX32(0x12345678u, mock_io_last_write(NVME_STATUS_REG_ADDR, &found));
	TEST_ASSERT_TRUE(found);
	TEST_ASSERT_EQUAL_HEX32(0, IO_READ32(PCIE_STATUS_REG_ADDR));
}

static uint32_t always_7(uintptr_t addr, uint32_t stored, void *ctx)
{
	(void)addr; (void)stored;
	return 7u + *(uint32_t *)ctx;
}

static void test_io_mock_read_handler_overrides_stored_value(void)
{
	uint32_t bias = 3;
	mock_io_set_reg(PCIE_FUNC_REG_ADDR, 99);
	mock_io_set_read_handler(PCIE_FUNC_REG_ADDR, always_7, &bias);
	TEST_ASSERT_EQUAL_UINT32(10, IO_READ32(PCIE_FUNC_REG_ADDR));
	mock_io_set_read_handler(PCIE_FUNC_REG_ADDR, NULL, NULL);
	TEST_ASSERT_EQUAL_UINT32(99, IO_READ32(PCIE_FUNC_REG_ADDR));
}

static void test_nsc_mock_default_is_ideal_nand(void)
{
	unsigned int status = 0, completion = 0, err[2] = {0, 0};
	V2FMCRegisters *dev = (V2FMCRegisters *)(uintptr_t)NSC_0_BASEADDR;

	TEST_ASSERT_EQUAL_UINT(0, V2FIsControllerBusy(dev));
	TEST_ASSERT_TRUE(V2FWayReady(V2FReadyBusyAsync(dev), 5));
	V2FStatusCheckAsync(dev, 2, &status);
	TEST_ASSERT_TRUE(V2FRequestReportDone(status));
	TEST_ASSERT_TRUE(V2FRequestComplete(V2FEliminateReportDoneFlag(status)));
	TEST_ASSERT_FALSE(V2FRequestFail(V2FEliminateReportDoneFlag(status)));
	V2FReadPageTransferAsync(dev, 2, NULL, NULL, err, &completion, 0x1234);
	TEST_ASSERT_TRUE(V2FTransferComplete(completion));
	TEST_ASSERT_TRUE(V2FCrcValid(err[0]));
	TEST_ASSERT_TRUE(V2FSpareChunkValid(err[0]));
	TEST_ASSERT_TRUE(V2FPageChunkValid(err[1]));
	TEST_ASSERT_EQUAL_size_t(4, mock_nsc_call_count());
	TEST_ASSERT_EQUAL_INT(MOCK_NSC_OP_READ_TRANSFER, mock_nsc_call_at(3)->op);
	TEST_ASSERT_EQUAL_UINT(0x1234, mock_nsc_call_at(3)->rowAddress);
}

static void test_nsc_mock_failure_injection(void)
{
	unsigned int status = 0;
	V2FMCRegisters *dev = (V2FMCRegisters *)(uintptr_t)NSC_1_BASEADDR;
	mock_nsc_set_status_report(dev, 3, MOCK_NSC_STATUS_FAIL);
	mock_nsc_set_controller_busy(dev, 1);
	V2FStatusCheckAsync(dev, 3, &status);
	TEST_ASSERT_TRUE(V2FRequestFail(V2FEliminateReportDoneFlag(status)));
	TEST_ASSERT_EQUAL_UINT(1, V2FIsControllerBusy(dev));
	/* other way on the same channel unaffected */
	V2FStatusCheckAsync(dev, 4, &status);
	TEST_ASSERT_FALSE(V2FRequestFail(V2FEliminateReportDoneFlag(status)));
}

static void test_firmware_assert_is_capturable(void)
{
	FTL_TEST_EXPECT_ASSERT(assert(!"deliberate"));
	TEST_ASSERT_EQUAL_STRING("!\"deliberate\"", ftl_test_last_assert_expr);
	FTL_TEST_EXPECT_ASSERT(ASSERT(1 == 2));
}

static void test_init_ftl_boots_against_mocked_nand(void)
{
	ftl_test_env_init_ftl();

	/* every die got a reset + set-feature during InitNandArray */
	TEST_ASSERT_EQUAL_size_t(USER_DIES, mock_nsc_count_op(MOCK_NSC_OP_RESET));
	TEST_ASSERT_EQUAL_size_t(USER_DIES, mock_nsc_count_op(MOCK_NSC_OP_SET_FEATURES));
	/* all request slots are back on the free list once boot has quiesced */
	TEST_ASSERT_EQUAL_UINT(AVAILABLE_OUNTSTANDING_REQ_COUNT, freeReqQ.reqCnt);
	TEST_ASSERT_EQUAL_UINT(0, notCompletedNandReqCnt);
	TEST_ASSERT_EQUAL_UINT(0, blockedReqCnt);
	TEST_ASSERT_TRUE(storageCapacity_L > 0);
	TEST_ASSERT_TRUE(ftl_test_printf_count > 0);
}

int main(void)
{
	UNITY_BEGIN();
	RUN_TEST(test_dram_window_is_mapped_and_zeroed);
	RUN_TEST(test_memory_map_fits_in_dram_window);
	RUN_TEST(test_io_mock_records_writes_and_serves_reads);
	RUN_TEST(test_io_mock_read_handler_overrides_stored_value);
	RUN_TEST(test_nsc_mock_default_is_ideal_nand);
	RUN_TEST(test_nsc_mock_failure_injection);
	RUN_TEST(test_firmware_assert_is_capturable);
	RUN_TEST(test_init_ftl_boots_against_mocked_nand);
	return UNITY_END();
}
