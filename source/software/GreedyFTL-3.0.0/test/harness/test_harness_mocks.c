/* Self-tests for the host harness: DRAM window, assert capture, mock_io, mock_nsc. */
#include "unity.h"

#include "fw_test.h"
#include "address_translation.h"
#include "ftl_config.h"
#include "memory_map.h"
#include "nvme/host_lld.h"
#include "xil_printf.h"

void setUp(void) { fw_test_reset(); }
void tearDown(void) {}

static void test_dram_window_is_mapped_and_zeroed_between_tests(void)
{
	volatile unsigned int *word = fw_ptr(DATA_BUFFER_BASE_ADDR);

	TEST_ASSERT_EQUAL_HEX32(0, *word);
	*word = 0xA5A5A5A5U;
	TEST_ASSERT_EQUAL_HEX32(0xA5A5A5A5U, *word);
	fw_memory_reset();
	TEST_ASSERT_EQUAL_HEX32(0, *word);
}

static void test_ftl_metadata_fits_in_dram_window(void)
{
	TEST_ASSERT_TRUE(FTL_MANAGEMENT_END_ADDR < FW_DRAM_END);
}

static void test_firmware_assert_is_captured(void)
{
	FW_EXPECT_ASSERT(fw_assert_fail("file.c", 42, "x == y"));
	TEST_ASSERT_EQUAL_UINT(1, fw_assert_count());
	TEST_ASSERT_EQUAL_STRING("file.c", fw_assert_last_file());
	TEST_ASSERT_EQUAL_INT(42, fw_assert_last_line());
}

static void test_io_macros_are_captured_by_mock_io(void)
{
	IO_WRITE32(NVME_STATUS_REG_ADDR, 0x12345678U);
	TEST_ASSERT_EQUAL_HEX32(0x12345678U, IO_READ32(NVME_STATUS_REG_ADDR));

	TEST_ASSERT_EQUAL_UINT(2, mock_io_log_count());
	TEST_ASSERT_EQUAL(MOCK_IO_WRITE, mock_io_log_at(0)->dir);
	TEST_ASSERT_EQUAL(MOCK_IO_READ, mock_io_log_at(1)->dir);
	TEST_ASSERT_EQUAL_UINT(1, mock_io_write_count(NVME_STATUS_REG_ADDR));
	TEST_ASSERT_EQUAL_HEX32(0x12345678U, mock_io_last_write(NVME_STATUS_REG_ADDR)->value);
}

static void test_mock_io_queued_reads_take_priority(void)
{
	mock_io_set_reg(0x1000U, 7);
	mock_io_queue_read(0x1000U, 1);
	mock_io_queue_read(0x1000U, 2);

	TEST_ASSERT_EQUAL_UINT(1, IO_READ32(0x1000U));
	TEST_ASSERT_EQUAL_UINT(2, IO_READ32(0x1000U));
	TEST_ASSERT_EQUAL_UINT(7, IO_READ32(0x1000U));
}

static void test_ftl_initialises_against_ideal_nand(void)
{
	InitFTL();

	TEST_ASSERT_EQUAL_UINT(0, fw_assert_count());
	TEST_ASSERT_TRUE(mock_nsc_count_cmd(V2FCommand_Reset) >= USER_DIES);
	TEST_ASSERT_TRUE(mock_nsc_call_count() > MOCK_NSC_MAX_CALLS);
}

static void test_nsc_counters_survive_full_call_log(void)
{
	fw_test_init_ftl();
	TEST_ASSERT_EQUAL_UINT(0, mock_nsc_call_count());

	V2FEraseBlockAsync(chCtlReg[0], 0, 0x80U);

	TEST_ASSERT_EQUAL_UINT(1, mock_nsc_count_cmd(V2FCommand_BlockErase));
	TEST_ASSERT_EQUAL_UINT(0x80U, mock_nsc_last_call()->rowAddress);
}

static void test_fixture_rewinds_die_allocator_and_inbyte(void)
{
	stub_inbyte_set('X');
	(void)FindDieForFreeSliceAllocation();

	fw_test_reset();

	TEST_ASSERT_EQUAL_UINT(0, FindDieForFreeSliceAllocation());
	TEST_ASSERT_EQUAL_INT('\r', inbyte());
}

static void test_mock_direct_dma_tracks_fifo(void)
{
	set_direct_tx_dma(0x10000000U, 0, 0x1000U, 4096);
	set_direct_rx_dma(0x10001000U, 0, 0x2000U, 4096);

	TEST_ASSERT_EQUAL_UINT(1, g_hostDmaStatus.fifoTail.directDmaTx);
	TEST_ASSERT_EQUAL_UINT(1, g_hostDmaStatus.fifoTail.directDmaRx);
	check_direct_tx_dma_done();
	check_direct_rx_dma_done();
	TEST_ASSERT_EQUAL_UINT(1, g_hostDmaStatus.fifoHead.directDmaTx);
	TEST_ASSERT_EQUAL_UINT(1, g_hostDmaStatus.fifoHead.directDmaRx);
}

int main(void)
{
	UNITY_BEGIN();
	RUN_TEST(test_dram_window_is_mapped_and_zeroed_between_tests);
	RUN_TEST(test_ftl_metadata_fits_in_dram_window);
	RUN_TEST(test_firmware_assert_is_captured);
	RUN_TEST(test_io_macros_are_captured_by_mock_io);
	RUN_TEST(test_mock_io_queued_reads_take_priority);
	RUN_TEST(test_ftl_initialises_against_ideal_nand);
	RUN_TEST(test_nsc_counters_survive_full_call_log);
	RUN_TEST(test_fixture_rewinds_die_allocator_and_inbyte);
	RUN_TEST(test_mock_direct_dma_tracks_fifo);
	return UNITY_END();
}
