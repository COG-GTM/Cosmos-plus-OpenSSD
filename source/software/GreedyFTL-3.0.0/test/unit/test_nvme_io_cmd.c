/* Unit tests for nvme/nvme_io_cmd.c: read/write/flush dispatch and validation. */
#include "unity.h"

#include <string.h>

#include "fw_test.h"
#include "memory_map.h"
#include "ftl_config.h"
#include "request_allocation.h"
#include "request_transform.h"
#include "nvme/nvme.h"
#include "nvme/nvme_io_cmd.h"
#include "nvme/nvme_main.h"

extern volatile NVME_CONTEXT g_nvmeTask;

/* Defined in nvme_io_cmd.c but not exported by nvme_io_cmd.h. */
void handle_nvme_io_read(unsigned int cmdSlotTag, NVME_IO_COMMAND *nvmeIOCmd);
void handle_nvme_io_write(unsigned int cmdSlotTag, NVME_IO_COMMAND *nvmeIOCmd);

#define PRP_ALIGNED 0x10000000U
#define PRP_MISALIGNED 0x10000008U
#define PRP_HIGH_TOO_LARGE 0x10U

void setUp(void)
{
	fw_test_reset();
	fw_test_init_ftl();
}

void tearDown(void) {}

static NVME_COMMAND make_io_cmd(unsigned char opc, unsigned short cmdSlotTag)
{
	NVME_COMMAND cmd;
	NVME_IO_COMMAND *io = (NVME_IO_COMMAND *)cmd.cmdDword;

	memset(&cmd, 0, sizeof(cmd));
	cmd.qID = 1;
	cmd.cmdSlotTag = cmdSlotTag;
	io->OPC = opc;
	io->PRP1[0] = PRP_ALIGNED;
	io->PRP2[0] = PRP_ALIGNED;
	return cmd;
}

static NVME_IO_COMMAND *io_of(NVME_COMMAND *cmd) { return (NVME_IO_COMMAND *)cmd->cmdDword; }

static void set_lba_range(NVME_COMMAND *cmd, unsigned int startLbaLow, unsigned int startLbaHigh,
		unsigned int nlbZeroBased)
{
	NVME_IO_COMMAND *io = io_of(cmd);

	io->dword[10] = startLbaLow;
	io->dword[11] = startLbaHigh;
	io->dword[12] = nlbZeroBased;
}

static unsigned int head_slice_req_code(void) { return reqPoolPtr->reqPool[sliceReqQ.headReq].reqCode; }

/* ---- flush ---- */

static void test_flush_posts_successful_auto_completion(void)
{
	NVME_COMMAND cmd = make_io_cmd(IO_NVM_FLUSH, 5);

	handle_nvme_io_cmd(&cmd);

	TEST_ASSERT_EQUAL_UINT(1, mock_host_count(MOCK_HOST_SET_AUTO_NVME_CPL));
	TEST_ASSERT_EQUAL_UINT(5, mock_host_last(MOCK_HOST_SET_AUTO_NVME_CPL)->args[0]);
	TEST_ASSERT_EQUAL_UINT(0, mock_host_last(MOCK_HOST_SET_AUTO_NVME_CPL)->args[1]);
	TEST_ASSERT_EQUAL_UINT(0, mock_host_last(MOCK_HOST_SET_AUTO_NVME_CPL)->args[2]);
	TEST_ASSERT_EQUAL_UINT(0, sliceReqQ.reqCnt);
}

/* ---- read ---- */

static void test_read_single_block_creates_one_read_slice_request(void)
{
	NVME_COMMAND cmd = make_io_cmd(IO_NVM_READ, 3);
	set_lba_range(&cmd, 0, 0, 0);

	handle_nvme_io_cmd(&cmd);

	TEST_ASSERT_EQUAL_UINT(1, sliceReqQ.reqCnt);
	TEST_ASSERT_EQUAL_UINT(REQ_CODE_READ, head_slice_req_code());
	TEST_ASSERT_EQUAL_UINT(3, reqPoolPtr->reqPool[sliceReqQ.headReq].nvmeCmdSlotTag);
	TEST_ASSERT_EQUAL_UINT(0, reqPoolPtr->reqPool[sliceReqQ.headReq].logicalSliceAddr);
	TEST_ASSERT_EQUAL_UINT(0, mock_host_count(MOCK_HOST_SET_AUTO_NVME_CPL));
}

static void test_read_maps_start_lba_to_logical_slice(void)
{
	NVME_COMMAND cmd = make_io_cmd(IO_NVM_READ, 0);
	set_lba_range(&cmd, 7 * NVME_BLOCKS_PER_SLICE, 0, 0);

	handle_nvme_io_read(0, io_of(&cmd));

	TEST_ASSERT_EQUAL_UINT(1, sliceReqQ.reqCnt);
	TEST_ASSERT_EQUAL_UINT(7, reqPoolPtr->reqPool[sliceReqQ.headReq].logicalSliceAddr);
}

static void test_read_spanning_slices_is_split(void)
{
	NVME_COMMAND cmd = make_io_cmd(IO_NVM_READ, 0);
	set_lba_range(&cmd, 0, 0, 3 * NVME_BLOCKS_PER_SLICE - 1);

	handle_nvme_io_cmd(&cmd);

	TEST_ASSERT_EQUAL_UINT(3, sliceReqQ.reqCnt);
}

static void test_read_ignores_prinfo_bits_above_nlb(void)
{
	NVME_COMMAND cmd = make_io_cmd(IO_NVM_READ, 0);
	/* NLB is the low 16 bits of DW12; PRINFO/FUA/LR live above it. */
	set_lba_range(&cmd, 0, 0, 0xC0000000U);

	handle_nvme_io_cmd(&cmd);

	TEST_ASSERT_EQUAL_UINT(1, sliceReqQ.reqCnt);
}

static void test_read_last_valid_lba_is_accepted(void)
{
	NVME_COMMAND cmd = make_io_cmd(IO_NVM_READ, 0);
	set_lba_range(&cmd, storageCapacity_L - 1, 0, 0);

	handle_nvme_io_cmd(&cmd);

	TEST_ASSERT_EQUAL_UINT(1, sliceReqQ.reqCnt);
}

static void test_read_start_lba_at_capacity_asserts(void)
{
	NVME_COMMAND cmd = make_io_cmd(IO_NVM_READ, 0);
	set_lba_range(&cmd, storageCapacity_L, 0, 0);

	FW_EXPECT_ASSERT(handle_nvme_io_cmd(&cmd));
	TEST_ASSERT_EQUAL_UINT(0, sliceReqQ.reqCnt);
}

static void test_read_nonzero_high_lba_dword_asserts(void)
{
	NVME_COMMAND cmd = make_io_cmd(IO_NVM_READ, 0);
	set_lba_range(&cmd, 0, 1, 0);

	FW_EXPECT_ASSERT(handle_nvme_io_read(0, io_of(&cmd)));
}

static void test_read_misaligned_prp1_asserts(void)
{
	NVME_COMMAND cmd = make_io_cmd(IO_NVM_READ, 0);
	io_of(&cmd)->PRP1[0] = PRP_MISALIGNED;

	FW_EXPECT_ASSERT(handle_nvme_io_read(0, io_of(&cmd)));
}

static void test_read_misaligned_prp2_asserts(void)
{
	NVME_COMMAND cmd = make_io_cmd(IO_NVM_READ, 0);
	io_of(&cmd)->PRP2[0] = PRP_MISALIGNED;

	FW_EXPECT_ASSERT(handle_nvme_io_read(0, io_of(&cmd)));
}

static void test_read_prp1_high_dword_too_large_asserts(void)
{
	NVME_COMMAND cmd = make_io_cmd(IO_NVM_READ, 0);
	io_of(&cmd)->PRP1[1] = PRP_HIGH_TOO_LARGE;

	FW_EXPECT_ASSERT(handle_nvme_io_read(0, io_of(&cmd)));
}

static void test_read_prp2_high_dword_too_large_asserts(void)
{
	NVME_COMMAND cmd = make_io_cmd(IO_NVM_READ, 0);
	io_of(&cmd)->PRP2[1] = PRP_HIGH_TOO_LARGE;

	FW_EXPECT_ASSERT(handle_nvme_io_read(0, io_of(&cmd)));
}

static void test_read_prp_high_dword_just_below_limit_is_accepted(void)
{
	NVME_COMMAND cmd = make_io_cmd(IO_NVM_READ, 0);
	io_of(&cmd)->PRP1[1] = PRP_HIGH_TOO_LARGE - 1;
	io_of(&cmd)->PRP2[1] = PRP_HIGH_TOO_LARGE - 1;

	handle_nvme_io_read(0, io_of(&cmd));

	TEST_ASSERT_EQUAL_UINT(1, sliceReqQ.reqCnt);
}

/* ---- write ---- */

static void test_write_single_block_creates_one_write_slice_request(void)
{
	NVME_COMMAND cmd = make_io_cmd(IO_NVM_WRITE, 9);
	set_lba_range(&cmd, 0, 0, 0);

	handle_nvme_io_cmd(&cmd);

	TEST_ASSERT_EQUAL_UINT(1, sliceReqQ.reqCnt);
	TEST_ASSERT_EQUAL_UINT(REQ_CODE_WRITE, head_slice_req_code());
	TEST_ASSERT_EQUAL_UINT(9, reqPoolPtr->reqPool[sliceReqQ.headReq].nvmeCmdSlotTag);
}

static void test_write_maps_start_lba_to_logical_slice(void)
{
	NVME_COMMAND cmd = make_io_cmd(IO_NVM_WRITE, 0);
	set_lba_range(&cmd, 11 * NVME_BLOCKS_PER_SLICE, 0, 0);

	handle_nvme_io_write(0, io_of(&cmd));

	TEST_ASSERT_EQUAL_UINT(11, reqPoolPtr->reqPool[sliceReqQ.headReq].logicalSliceAddr);
}

static void test_write_spanning_slices_is_split(void)
{
	NVME_COMMAND cmd = make_io_cmd(IO_NVM_WRITE, 0);
	set_lba_range(&cmd, 0, 0, 2 * NVME_BLOCKS_PER_SLICE - 1);

	handle_nvme_io_cmd(&cmd);

	TEST_ASSERT_EQUAL_UINT(2, sliceReqQ.reqCnt);
}

static void test_write_with_fua_bit_is_accepted(void)
{
	NVME_COMMAND cmd = make_io_cmd(IO_NVM_WRITE, 0);
	set_lba_range(&cmd, 0, 0, 1U << 30);

	handle_nvme_io_cmd(&cmd);

	TEST_ASSERT_EQUAL_UINT(1, sliceReqQ.reqCnt);
	TEST_ASSERT_EQUAL_UINT(REQ_CODE_WRITE, head_slice_req_code());
}

static void test_write_start_lba_at_capacity_asserts(void)
{
	NVME_COMMAND cmd = make_io_cmd(IO_NVM_WRITE, 0);
	set_lba_range(&cmd, storageCapacity_L, 0, 0);

	FW_EXPECT_ASSERT(handle_nvme_io_cmd(&cmd));
	TEST_ASSERT_EQUAL_UINT(0, sliceReqQ.reqCnt);
}

static void test_write_nonzero_high_lba_dword_asserts(void)
{
	NVME_COMMAND cmd = make_io_cmd(IO_NVM_WRITE, 0);
	set_lba_range(&cmd, 0, 0xFFFFFFFFU, 0);

	FW_EXPECT_ASSERT(handle_nvme_io_write(0, io_of(&cmd)));
}

static void test_write_misaligned_prp1_asserts(void)
{
	NVME_COMMAND cmd = make_io_cmd(IO_NVM_WRITE, 0);
	io_of(&cmd)->PRP1[0] = PRP_MISALIGNED;

	FW_EXPECT_ASSERT(handle_nvme_io_write(0, io_of(&cmd)));
}

static void test_write_misaligned_prp2_asserts(void)
{
	NVME_COMMAND cmd = make_io_cmd(IO_NVM_WRITE, 0);
	io_of(&cmd)->PRP2[0] = PRP_MISALIGNED;

	FW_EXPECT_ASSERT(handle_nvme_io_write(0, io_of(&cmd)));
}

static void test_write_prp1_high_dword_too_large_asserts(void)
{
	NVME_COMMAND cmd = make_io_cmd(IO_NVM_WRITE, 0);
	io_of(&cmd)->PRP1[1] = PRP_HIGH_TOO_LARGE;

	FW_EXPECT_ASSERT(handle_nvme_io_write(0, io_of(&cmd)));
}

static void test_write_prp2_high_dword_too_large_asserts(void)
{
	NVME_COMMAND cmd = make_io_cmd(IO_NVM_WRITE, 0);
	io_of(&cmd)->PRP2[1] = PRP_HIGH_TOO_LARGE;

	FW_EXPECT_ASSERT(handle_nvme_io_write(0, io_of(&cmd)));
}

/* ---- unsupported opcodes ---- */

static void test_write_uncorrectable_opcode_asserts(void)
{
	NVME_COMMAND cmd = make_io_cmd(IO_NVM_WRITE_UNCORRECTABLE, 0);

	FW_EXPECT_ASSERT(handle_nvme_io_cmd(&cmd));
	TEST_ASSERT_EQUAL_UINT(0, mock_host_count(MOCK_HOST_SET_AUTO_NVME_CPL));
}

static void test_compare_opcode_asserts(void)
{
	NVME_COMMAND cmd = make_io_cmd(IO_NVM_COMPARE, 0);

	FW_EXPECT_ASSERT(handle_nvme_io_cmd(&cmd));
}

static void test_dataset_management_opcode_asserts(void)
{
	NVME_COMMAND cmd = make_io_cmd(IO_NVM_DATASET_MANAGEMENT, 0);

	FW_EXPECT_ASSERT(handle_nvme_io_cmd(&cmd));
}

static void test_vendor_specific_opcode_asserts(void)
{
	NVME_COMMAND cmd = make_io_cmd(0xFF, 0);

	FW_EXPECT_ASSERT(handle_nvme_io_cmd(&cmd));
	TEST_ASSERT_EQUAL_UINT(0, sliceReqQ.reqCnt);
}

/* ---- range validation gaps ---- */

static void test_read_range_crossing_end_of_capacity_is_rejected(void)
{
	TEST_IGNORE_MESSAGE("BUG: only the start LBA is range checked; startLba + nlb may exceed "
			"storageCapacity_L and is forwarded to ReqTransNvmeToSlice");
	/* Expected: an out-of-range read (start valid, end past capacity) is
	 * rejected (assert or LBA Out of Range status). Actual: the command is
	 * translated into slice requests for logical slices beyond the SSD. */
	NVME_COMMAND cmd = make_io_cmd(IO_NVM_READ, 0);
	set_lba_range(&cmd, storageCapacity_L - 1, 0, 2 * NVME_BLOCKS_PER_SLICE - 1);

	FW_EXPECT_ASSERT(handle_nvme_io_cmd(&cmd));
}

static void test_read_nlb_above_max_is_rejected(void)
{
	TEST_IGNORE_MESSAGE("BUG: the nlb < MAX_NUM_OF_NLB check is commented out, so a read "
			"larger than MDTS is accepted");
	/* Expected: nlb >= MAX_NUM_OF_NLB (MDTS) trips an assert.
	 * Actual: the oversized request is split into slice requests. */
	NVME_COMMAND cmd = make_io_cmd(IO_NVM_READ, 0);
	set_lba_range(&cmd, 0, 0, MAX_NUM_OF_NLB);

	FW_EXPECT_ASSERT(handle_nvme_io_cmd(&cmd));
}

int main(void)
{
	UNITY_BEGIN();
	RUN_TEST(test_flush_posts_successful_auto_completion);

	RUN_TEST(test_read_single_block_creates_one_read_slice_request);
	RUN_TEST(test_read_maps_start_lba_to_logical_slice);
	RUN_TEST(test_read_spanning_slices_is_split);
	RUN_TEST(test_read_ignores_prinfo_bits_above_nlb);
	RUN_TEST(test_read_last_valid_lba_is_accepted);
	RUN_TEST(test_read_start_lba_at_capacity_asserts);
	RUN_TEST(test_read_nonzero_high_lba_dword_asserts);
	RUN_TEST(test_read_misaligned_prp1_asserts);
	RUN_TEST(test_read_misaligned_prp2_asserts);
	RUN_TEST(test_read_prp1_high_dword_too_large_asserts);
	RUN_TEST(test_read_prp2_high_dword_too_large_asserts);
	RUN_TEST(test_read_prp_high_dword_just_below_limit_is_accepted);

	RUN_TEST(test_write_single_block_creates_one_write_slice_request);
	RUN_TEST(test_write_maps_start_lba_to_logical_slice);
	RUN_TEST(test_write_spanning_slices_is_split);
	RUN_TEST(test_write_with_fua_bit_is_accepted);
	RUN_TEST(test_write_start_lba_at_capacity_asserts);
	RUN_TEST(test_write_nonzero_high_lba_dword_asserts);
	RUN_TEST(test_write_misaligned_prp1_asserts);
	RUN_TEST(test_write_misaligned_prp2_asserts);
	RUN_TEST(test_write_prp1_high_dword_too_large_asserts);
	RUN_TEST(test_write_prp2_high_dword_too_large_asserts);

	RUN_TEST(test_write_uncorrectable_opcode_asserts);
	RUN_TEST(test_compare_opcode_asserts);
	RUN_TEST(test_dataset_management_opcode_asserts);
	RUN_TEST(test_vendor_specific_opcode_asserts);

	RUN_TEST(test_read_range_crossing_end_of_capacity_is_rejected);
	RUN_TEST(test_read_nlb_above_max_is_rejected);
	return UNITY_END();
}
