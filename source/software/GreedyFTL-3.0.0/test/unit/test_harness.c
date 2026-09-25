/* Proves the host harness itself: emulated DRAM window, IO mock, NSC mock,
 * assert capture, and a full InitFTL() boot against the mocked NAND array. */
#include <string.h>
#include "unity.h"
#include "ftl_test_env.h"
#include "memory_map.h"
#include "nvme/host_lld.h"
#include "request_allocation.h"
#include "address_translation.h"

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

static void test_nsc_mock_stores_programmed_pages_until_erase(void)
{
	V2FMCRegisters *dev = (V2FMCRegisters *)(uintptr_t)XPAR_TIGER4NSC_0_BASEADDR;
	static unsigned char page[BYTES_PER_DATA_REGION_OF_PAGE];
	static unsigned char spare[BYTES_PER_SPARE_REGION_OF_PAGE];
	static unsigned char out[BYTES_PER_DATA_REGION_OF_PAGE];
	static unsigned char outSpare[BYTES_PER_SPARE_REGION_OF_PAGE];
	unsigned int err[2], done = 0;
	unsigned int row = 17 * PAGES_PER_MLC_BLOCK + 3;

	memset(page, 0xA5, sizeof(page));
	memset(spare, 0x5A, sizeof(spare));
	TEST_ASSERT_NULL(mock_nsc_page_data(dev, 2, row));

	V2FProgramPageAsync(dev, 2, row, page, spare);
	TEST_ASSERT_NOT_NULL(mock_nsc_page_data(dev, 2, row));
	TEST_ASSERT_NULL(mock_nsc_page_data(dev, 3, row));

	V2FReadPageTriggerAsync(dev, 2, row);
	V2FReadPageTransferAsync(dev, 2, out, outSpare, err, &done, row);
	TEST_ASSERT_EQUAL_MEMORY(page, out, sizeof(page));
	TEST_ASSERT_EQUAL_MEMORY(spare, outSpare, sizeof(spare));
	TEST_ASSERT_EQUAL_UINT(1, done);

	V2FEraseBlockAsync(dev, 2, 17 * PAGES_PER_MLC_BLOCK);
	TEST_ASSERT_NULL(mock_nsc_page_data(dev, 2, row));
	memset(out, 0, sizeof(out));
	memset(outSpare, 0, sizeof(outSpare));
	V2FReadPageTransferAsync(dev, 2, out, outSpare, err, &done, row);
	TEST_ASSERT_EACH_EQUAL_UINT8(0xFF, out, sizeof(out));
	TEST_ASSERT_EACH_EQUAL_UINT8(0xFF, outSpare, sizeof(outSpare));
}

static void test_nsc_mock_raw_read_returns_data_then_spare_row(void)
{
	V2FMCRegisters *dev = (V2FMCRegisters *)(uintptr_t)XPAR_TIGER4NSC_1_BASEADDR;
	static unsigned char page[BYTES_PER_DATA_REGION_OF_PAGE];
	static unsigned char spare[BYTES_PER_SPARE_REGION_OF_PAGE];
	static unsigned char raw[BYTES_PER_NAND_ROW];
	static unsigned char rowSrc[BYTES_PER_NAND_ROW];
	unsigned int done = 0;

	memset(raw, 0, sizeof(raw));
	V2FReadPageTriggerAsync(dev, 5, 40);
	V2FReadPageTransferRawAsync(dev, 5, raw, &done);
	TEST_ASSERT_EQUAL_UINT8(0xFF, raw[BAD_BLOCK_MARK_BYTE0]);
	TEST_ASSERT_EQUAL_UINT8(0xFF, raw[BAD_BLOCK_MARK_BYTE1]);
	TEST_ASSERT_EQUAL_UINT8(0xFF, raw[BYTES_PER_NAND_ROW - 1]);

	memset(page, 0x11, sizeof(page));
	memset(spare, 0x00, sizeof(spare));
	V2FProgramPageAsync(dev, 5, 40, page, spare);
	V2FReadPageTriggerAsync(dev, 5, 40);
	V2FReadPageTransferRawAsync(dev, 5, raw, &done);
	TEST_ASSERT_EQUAL_UINT8(0x11, raw[BAD_BLOCK_MARK_BYTE0]);
	TEST_ASSERT_EQUAL_UINT8(0x00, raw[BAD_BLOCK_MARK_BYTE1]);
	TEST_ASSERT_EQUAL_UINT8(0xFF, raw[BYTES_PER_NAND_ROW - 1]);

	/* An oversized override is clamped to the destination. */
	memset(rowSrc, 0x77, sizeof(rowSrc));
	memset(page, 0, sizeof(page));
	mock_nsc_set_read_page_source(dev, 5, rowSrc, sizeof(rowSrc));
	{
		static unsigned char guarded[BYTES_PER_DATA_REGION_OF_PAGE + 16];
		memset(guarded, 0xEE, sizeof(guarded));
		V2FReadPageTransferAsync(dev, 5, guarded, spare, NULL, &done, 40);
		TEST_ASSERT_EQUAL_UINT8(0x77, guarded[BYTES_PER_DATA_REGION_OF_PAGE - 1]);
		TEST_ASSERT_EQUAL_UINT8(0xEE, guarded[BYTES_PER_DATA_REGION_OF_PAGE]);
	}
}

static void test_default_boot_builds_bad_block_table_on_erased_nand(void)
{
	unsigned int dieNo, blockNo, bad = 0;
	ftl_test_env_init_ftl();
	/* Erased NAND has no table: FindBadBlock() scans and SaveBadBlockTable()
	 * programs one table page per die. Every block scans clean (0xFF marks). */
	TEST_ASSERT_TRUE(mock_nsc_count_op(MOCK_NSC_OP_PROGRAM) >= (size_t)USER_DIES);
	for (dieNo = 0; dieNo < USER_DIES; dieNo++)
		for (blockNo = 0; blockNo < TOTAL_BLOCKS_PER_DIE; blockNo++)
			if (phyBlockMapPtr->phyBlock[dieNo][blockNo].bad &&
			    blockNo != bbtInfoMapPtr->bbtInfo[dieNo].phyBlock)
				bad++;
	TEST_ASSERT_EQUAL_UINT(0, bad);
	TEST_ASSERT_NOT_NULL(mock_nsc_page_data(
		(V2FMCRegisters *)(uintptr_t)XPAR_TIGER4NSC_0_BASEADDR, 0,
		bbtInfoMapPtr->bbtInfo[0].phyBlock * PAGES_PER_MLC_BLOCK
			+ PlsbPage2VpageTranslation(START_PAGE_NO_OF_BAD_BLOCK_TABLE_BLOCK)));
}

static void test_io_mock_write_log_grows_past_initial_capacity(void)
{
	size_t i;
	int found = 0;
	for (i = 0; i < 5000; i++)
		mock_io_write32(0x43C00000u, (uint32_t)i);
	mock_io_write32(0x43C00004u, 0x55u);
	TEST_ASSERT_EQUAL_size_t(5001, mock_io_write_count());
	TEST_ASSERT_EQUAL_size_t(5000, mock_io_write_count_for(0x43C00000u));
	TEST_ASSERT_EQUAL_HEX32(0x55u, mock_io_last_write(0x43C00004u, &found));
	TEST_ASSERT_TRUE(found);
	TEST_ASSERT_EQUAL_HEX32(4999u, mock_io_write_at(4999)->value);
}

static void test_init_ftl_with_console_x_erases_whole_array(void)
{
	ftl_test_env_init_ftl_with_console("X");
	/* 'X' runs EraseTotalBlockSpace(): one erase per block of every die. */
	TEST_ASSERT_TRUE(mock_nsc_count_op(MOCK_NSC_OP_ERASE) >= (size_t)USER_DIES * TOTAL_BLOCKS_PER_DIE);
	TEST_ASSERT_EQUAL_UINT(0, notCompletedNandReqCnt);
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
	RUN_TEST(test_nsc_mock_stores_programmed_pages_until_erase);
	RUN_TEST(test_nsc_mock_raw_read_returns_data_then_spare_row);
	RUN_TEST(test_default_boot_builds_bad_block_table_on_erased_nand);
	RUN_TEST(test_io_mock_write_log_grows_past_initial_capacity);
	RUN_TEST(test_init_ftl_with_console_x_erases_whole_array);
	RUN_TEST(test_firmware_assert_is_capturable);
	RUN_TEST(test_init_ftl_boots_against_mocked_nand);
	return UNITY_END();
}
