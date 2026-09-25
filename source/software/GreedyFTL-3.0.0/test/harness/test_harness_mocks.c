/* Self-tests for the host harness: DRAM window, assert capture, mock_io, mock_nsc. */
#include "unity.h"

#include "fw_test.h"
#include "memory_map.h"
#include "nvme/host_lld.h"

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
	fw_test_init_ftl();

	TEST_ASSERT_EQUAL_UINT(0, fw_assert_count());
	TEST_ASSERT_TRUE(mock_nsc_count_cmd(V2FCommand_Reset) >= USER_DIES);
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
	return UNITY_END();
}
