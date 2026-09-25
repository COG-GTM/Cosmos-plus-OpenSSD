/* Unit tests for nvme/nvme_io_cmd.c: decoding of NVMe read/write/flush
 * commands into slice requests, the LBA/PRP ASSERT guards, the unsupported
 * opcode path and the completion FIFO traffic captured by mock_io. */
#include <string.h>
#include "unity.h"
#include "ftl_test_env.h"
#include "ftl_config.h"
#include "request_allocation.h"
#include "request_format.h"
#include "nvme/nvme.h"
#include "nvme/host_lld.h"
#include "nvme/nvme_io_cmd.h"

#define TEST_SLOT_TAG        7u
#define TEST_PRP1            0x20000000u
#define TEST_PRP2            0x20001000u
#define CPL_FIFO_DWORD1_ADDR (NVME_CPL_FIFO_REG_ADDR + 4)
#define CPL_FIFO_DWORD2_ADDR (NVME_CPL_FIFO_REG_ADDR + 8)

void setUp(void) { ftl_test_env_reset(); ftl_test_env_init_ftl(); }
void tearDown(void) {}

static NVME_COMMAND cmd;
static NVME_IO_COMMAND *io;

static void build_io_cmd(unsigned int opc, unsigned int startLba, unsigned int nlbZeroBased)
{
	memset(&cmd, 0, sizeof(cmd));
	io = (NVME_IO_COMMAND *)cmd.cmdDword;
	cmd.qID = 1;
	cmd.cmdSlotTag = TEST_SLOT_TAG;
	io->OPC = opc;
	io->NSID = 1;
	io->PRP1[0] = TEST_PRP1;
	io->PRP2[0] = TEST_PRP2;
	io->dword10 = startLba;
	io->dword12 = nlbZeroBased;
}

static const SSD_REQ_FORMAT *slice_req_at(unsigned int index)
{
	unsigned int reqSlot = sliceReqQ.headReq;
	while (index--)
		reqSlot = reqPoolPtr->reqPool[reqSlot].nextReq;
	return &reqPoolPtr->reqPool[reqSlot];
}

/* ---------------------------------------------------------------- read --- */

static void test_read_single_slice_decodes_lba_and_tag(void)
{
	build_io_cmd(IO_NVM_READ, 0, NVME_BLOCKS_PER_SLICE - 1);

	handle_nvme_io_cmd(&cmd);

	TEST_ASSERT_EQUAL_UINT(1, sliceReqQ.reqCnt);
	TEST_ASSERT_EQUAL_UINT(REQ_TYPE_SLICE, slice_req_at(0)->reqType);
	TEST_ASSERT_EQUAL_UINT(REQ_CODE_READ, slice_req_at(0)->reqCode);
	TEST_ASSERT_EQUAL_UINT(TEST_SLOT_TAG, slice_req_at(0)->nvmeCmdSlotTag);
	TEST_ASSERT_EQUAL_UINT(0, slice_req_at(0)->logicalSliceAddr);
	TEST_ASSERT_EQUAL_UINT(0, slice_req_at(0)->nvmeDmaInfo.startIndex);
	TEST_ASSERT_EQUAL_UINT(0, slice_req_at(0)->nvmeDmaInfo.nvmeBlockOffset);
	TEST_ASSERT_EQUAL_UINT(NVME_BLOCKS_PER_SLICE, slice_req_at(0)->nvmeDmaInfo.numOfNvmeBlock);
}

static void test_read_two_slices_decodes_into_two_requests(void)
{
	build_io_cmd(IO_NVM_READ, 0, NVME_BLOCKS_PER_SLICE * 2 - 1);

	handle_nvme_io_cmd(&cmd);

	TEST_ASSERT_EQUAL_UINT(2, sliceReqQ.reqCnt);
	TEST_ASSERT_EQUAL_UINT(0, slice_req_at(0)->logicalSliceAddr);
	TEST_ASSERT_EQUAL_UINT(1, slice_req_at(1)->logicalSliceAddr);
	TEST_ASSERT_EQUAL_UINT(REQ_CODE_READ, slice_req_at(1)->reqCode);
	TEST_ASSERT_EQUAL_UINT(TEST_SLOT_TAG, slice_req_at(1)->nvmeCmdSlotTag);
}

static void test_read_unaligned_lba_splits_at_slice_boundary(void)
{
	/* Last block of slice 2 followed by the whole of slice 3. */
	build_io_cmd(IO_NVM_READ, NVME_BLOCKS_PER_SLICE * 3 - 1, NVME_BLOCKS_PER_SLICE);

	handle_nvme_io_cmd(&cmd);

	TEST_ASSERT_EQUAL_UINT(2, sliceReqQ.reqCnt);
	TEST_ASSERT_EQUAL_UINT(2, slice_req_at(0)->logicalSliceAddr);
	TEST_ASSERT_EQUAL_UINT(0, slice_req_at(0)->nvmeDmaInfo.startIndex);
	TEST_ASSERT_EQUAL_UINT(NVME_BLOCKS_PER_SLICE - 1, slice_req_at(0)->nvmeDmaInfo.nvmeBlockOffset);
	TEST_ASSERT_EQUAL_UINT(1, slice_req_at(0)->nvmeDmaInfo.numOfNvmeBlock);
	TEST_ASSERT_EQUAL_UINT(3, slice_req_at(1)->logicalSliceAddr);
	TEST_ASSERT_EQUAL_UINT(1, slice_req_at(1)->nvmeDmaInfo.startIndex);
	TEST_ASSERT_EQUAL_UINT(0, slice_req_at(1)->nvmeDmaInfo.nvmeBlockOffset);
	TEST_ASSERT_EQUAL_UINT(NVME_BLOCKS_PER_SLICE, slice_req_at(1)->nvmeDmaInfo.numOfNvmeBlock);
}

static void test_read_sub_slice_range_produces_single_partial_request(void)
{
	build_io_cmd(IO_NVM_READ, 2, 0);

	handle_nvme_io_cmd(&cmd);

	TEST_ASSERT_EQUAL_UINT(1, sliceReqQ.reqCnt);
	TEST_ASSERT_EQUAL_UINT(0, slice_req_at(0)->logicalSliceAddr);
	TEST_ASSERT_EQUAL_UINT(2, slice_req_at(0)->nvmeDmaInfo.nvmeBlockOffset);
	TEST_ASSERT_EQUAL_UINT(1, slice_req_at(0)->nvmeDmaInfo.numOfNvmeBlock);
}

static void test_read_with_trailing_partial_slice_covers_all_blocks(void)
{
	/* Slice 0 in full, then a single block of slice 1. */
	build_io_cmd(IO_NVM_READ, 0, NVME_BLOCKS_PER_SLICE);

	handle_nvme_io_cmd(&cmd);

	TEST_ASSERT_EQUAL_UINT(2, sliceReqQ.reqCnt);
	TEST_ASSERT_EQUAL_UINT(0, slice_req_at(0)->logicalSliceAddr);
	TEST_ASSERT_EQUAL_UINT(NVME_BLOCKS_PER_SLICE, slice_req_at(0)->nvmeDmaInfo.numOfNvmeBlock);
	TEST_ASSERT_EQUAL_UINT(1, slice_req_at(1)->logicalSliceAddr);
	TEST_ASSERT_EQUAL_UINT(NVME_BLOCKS_PER_SLICE, slice_req_at(1)->nvmeDmaInfo.startIndex);
	TEST_ASSERT_EQUAL_UINT(0, slice_req_at(1)->nvmeDmaInfo.nvmeBlockOffset);
	TEST_ASSERT_EQUAL_UINT(1, slice_req_at(1)->nvmeDmaInfo.numOfNvmeBlock);
}

static void test_read_spanning_three_slices_with_partial_ends(void)
{
	/* Last block of slice 1, all of slice 2, first two blocks of slice 3. */
	build_io_cmd(IO_NVM_READ, NVME_BLOCKS_PER_SLICE * 2 - 1, NVME_BLOCKS_PER_SLICE + 2);

	handle_nvme_io_cmd(&cmd);

	TEST_ASSERT_EQUAL_UINT(3, sliceReqQ.reqCnt);
	TEST_ASSERT_EQUAL_UINT(1, slice_req_at(0)->nvmeDmaInfo.numOfNvmeBlock);
	TEST_ASSERT_EQUAL_UINT(2, slice_req_at(1)->logicalSliceAddr);
	TEST_ASSERT_EQUAL_UINT(NVME_BLOCKS_PER_SLICE, slice_req_at(1)->nvmeDmaInfo.numOfNvmeBlock);
	TEST_ASSERT_EQUAL_UINT(3, slice_req_at(2)->logicalSliceAddr);
	TEST_ASSERT_EQUAL_UINT(NVME_BLOCKS_PER_SLICE + 1, slice_req_at(2)->nvmeDmaInfo.startIndex);
	TEST_ASSERT_EQUAL_UINT(2, slice_req_at(2)->nvmeDmaInfo.numOfNvmeBlock);
}

static void test_read_of_highest_lba_is_accepted(void)
{
	build_io_cmd(IO_NVM_READ, storageCapacity_L - 1, 0);

	handle_nvme_io_cmd(&cmd);

	TEST_ASSERT_EQUAL_UINT(1, sliceReqQ.reqCnt);
	TEST_ASSERT_EQUAL_UINT((storageCapacity_L - 1) / NVME_BLOCKS_PER_SLICE,
	                       slice_req_at(0)->logicalSliceAddr);
}

static void test_read_does_not_post_completion(void)
{
	build_io_cmd(IO_NVM_READ, 0, 0);

	handle_nvme_io_cmd(&cmd);

	TEST_ASSERT_EQUAL_size_t(0, mock_io_write_count_for(NVME_CPL_FIFO_REG_ADDR));
	TEST_ASSERT_EQUAL_size_t(0, mock_io_write_count_for(CPL_FIFO_DWORD1_ADDR));
	TEST_ASSERT_EQUAL_size_t(0, mock_io_write_count_for(CPL_FIFO_DWORD2_ADDR));
}

/* --------------------------------------------------------------- write --- */

static void test_write_single_slice_decodes_lba_and_tag(void)
{
	build_io_cmd(IO_NVM_WRITE, NVME_BLOCKS_PER_SLICE * 5, NVME_BLOCKS_PER_SLICE - 1);

	handle_nvme_io_cmd(&cmd);

	TEST_ASSERT_EQUAL_UINT(1, sliceReqQ.reqCnt);
	TEST_ASSERT_EQUAL_UINT(REQ_CODE_WRITE, slice_req_at(0)->reqCode);
	TEST_ASSERT_EQUAL_UINT(TEST_SLOT_TAG, slice_req_at(0)->nvmeCmdSlotTag);
	TEST_ASSERT_EQUAL_UINT(5, slice_req_at(0)->logicalSliceAddr);
}

static void test_write_three_slices_decodes_into_three_requests(void)
{
	build_io_cmd(IO_NVM_WRITE, 0, NVME_BLOCKS_PER_SLICE * 3 - 1);

	handle_nvme_io_cmd(&cmd);

	TEST_ASSERT_EQUAL_UINT(3, sliceReqQ.reqCnt);
	TEST_ASSERT_EQUAL_UINT(REQ_CODE_WRITE, slice_req_at(2)->reqCode);
	TEST_ASSERT_EQUAL_UINT(2, slice_req_at(2)->logicalSliceAddr);
}

static void test_write_ignores_fua_bit_in_dword12(void)
{
	IO_READ_COMMAND_DW12 dw12;

	dw12.dword = 0;
	dw12.NLB = 0;
	dw12.FUA = 1;
	build_io_cmd(IO_NVM_WRITE, 0, 0);
	io->dword12 = dw12.dword;

	handle_nvme_io_cmd(&cmd);

	TEST_ASSERT_EQUAL_UINT(1, sliceReqQ.reqCnt);
	TEST_ASSERT_EQUAL_UINT(REQ_CODE_WRITE, slice_req_at(0)->reqCode);
}

/* --------------------------------------------------------------- flush --- */

static void test_flush_posts_auto_completion_with_success_status(void)
{
	NVME_CPL_FIFO_REG cpl;
	int found;

	build_io_cmd(IO_NVM_FLUSH, 0, 0);

	handle_nvme_io_cmd(&cmd);

	TEST_ASSERT_EQUAL_UINT(0, sliceReqQ.reqCnt);
	/* set_auto_nvme_cpl only writes dword[1] (specific) and dword[2]. */
	TEST_ASSERT_EQUAL_size_t(0, mock_io_write_count_for(NVME_CPL_FIFO_REG_ADDR));
	TEST_ASSERT_EQUAL_size_t(1, mock_io_write_count_for(CPL_FIFO_DWORD1_ADDR));
	TEST_ASSERT_EQUAL_size_t(1, mock_io_write_count_for(CPL_FIFO_DWORD2_ADDR));

	cpl.dword[1] = mock_io_last_write(CPL_FIFO_DWORD1_ADDR, &found);
	TEST_ASSERT_TRUE(found);
	cpl.dword[2] = mock_io_last_write(CPL_FIFO_DWORD2_ADDR, &found);
	TEST_ASSERT_TRUE(found);

	TEST_ASSERT_EQUAL_UINT(0, cpl.specific);
	TEST_ASSERT_EQUAL_UINT(TEST_SLOT_TAG, cpl.cmdSlotTag);
	TEST_ASSERT_EQUAL_UINT(AUTO_CPL_TYPE, cpl.cplType);
	TEST_ASSERT_EQUAL_UINT(0, cpl.statusFieldWord);
}

static void test_flush_completion_carries_command_slot_tag(void)
{
	NVME_CPL_FIFO_REG cpl;
	int found;

	build_io_cmd(IO_NVM_FLUSH, 0, 0);
	cmd.cmdSlotTag = 0x5A;

	handle_nvme_io_cmd(&cmd);

	cpl.dword[2] = mock_io_last_write(CPL_FIFO_DWORD2_ADDR, &found);
	TEST_ASSERT_TRUE(found);
	TEST_ASSERT_EQUAL_UINT(0x5A, cpl.cmdSlotTag);
}

/* ----------------------------------------------------- ASSERT guards --- */

static void test_read_beyond_capacity_asserts(void)
{
	build_io_cmd(IO_NVM_READ, storageCapacity_L, 0);
	FTL_TEST_EXPECT_ASSERT(handle_nvme_io_cmd(&cmd));
	TEST_ASSERT_EQUAL_UINT(0, sliceReqQ.reqCnt);
}

static void test_write_beyond_capacity_asserts(void)
{
	build_io_cmd(IO_NVM_WRITE, storageCapacity_L + 100, 0);
	FTL_TEST_EXPECT_ASSERT(handle_nvme_io_cmd(&cmd));
	TEST_ASSERT_EQUAL_UINT(0, sliceReqQ.reqCnt);
}

static void test_read_with_nonzero_upper_lba_dword_asserts(void)
{
	build_io_cmd(IO_NVM_READ, 0, 0);
	io->dword11 = 1;
	FTL_TEST_EXPECT_ASSERT(handle_nvme_io_cmd(&cmd));
}

static void test_write_with_nonzero_upper_lba_dword_asserts(void)
{
	build_io_cmd(IO_NVM_WRITE, 0, 0);
	io->dword11 = 1;
	FTL_TEST_EXPECT_ASSERT(handle_nvme_io_cmd(&cmd));
}

static void test_read_with_misaligned_prp1_asserts(void)
{
	build_io_cmd(IO_NVM_READ, 0, 0);
	io->PRP1[0] = TEST_PRP1 + 0x8;
	FTL_TEST_EXPECT_ASSERT(handle_nvme_io_cmd(&cmd));
}

static void test_read_with_misaligned_prp2_asserts(void)
{
	build_io_cmd(IO_NVM_READ, 0, 0);
	io->PRP2[0] = TEST_PRP2 + 0x4;
	FTL_TEST_EXPECT_ASSERT(handle_nvme_io_cmd(&cmd));
}

static void test_write_with_misaligned_prp1_asserts(void)
{
	build_io_cmd(IO_NVM_WRITE, 0, 0);
	io->PRP1[0] = TEST_PRP1 + 0x1;
	FTL_TEST_EXPECT_ASSERT(handle_nvme_io_cmd(&cmd));
}

static void test_write_with_misaligned_prp2_asserts(void)
{
	build_io_cmd(IO_NVM_WRITE, 0, 0);
	io->PRP2[0] = TEST_PRP2 + 0xF;
	FTL_TEST_EXPECT_ASSERT(handle_nvme_io_cmd(&cmd));
}

static void test_read_with_prp_upper_address_out_of_range_asserts(void)
{
	build_io_cmd(IO_NVM_READ, 0, 0);
	io->PRP1[1] = 0x10;
	FTL_TEST_EXPECT_ASSERT(handle_nvme_io_cmd(&cmd));
}

static void test_write_with_prp_upper_address_out_of_range_asserts(void)
{
	build_io_cmd(IO_NVM_WRITE, 0, 0);
	io->PRP2[1] = 0x10;
	FTL_TEST_EXPECT_ASSERT(handle_nvme_io_cmd(&cmd));
}

static void test_prp_upper_address_below_limit_is_accepted(void)
{
	build_io_cmd(IO_NVM_READ, 0, 0);
	io->PRP1[1] = 0xF;
	io->PRP2[1] = 0xF;

	handle_nvme_io_cmd(&cmd);

	TEST_ASSERT_EQUAL_UINT(1, sliceReqQ.reqCnt);
}

/* ------------------------------------------------- unsupported opcode --- */

static void test_unsupported_opcode_asserts_without_completion(void)
{
	build_io_cmd(0x0D /* Dataset Management */, 0, 0);

	FTL_TEST_EXPECT_ASSERT(handle_nvme_io_cmd(&cmd));

	TEST_ASSERT_EQUAL_UINT(0, sliceReqQ.reqCnt);
	TEST_ASSERT_EQUAL_size_t(0, mock_io_write_count());
}

static void test_write_uncorrectable_opcode_is_unsupported(void)
{
	build_io_cmd(0x04 /* Write Uncorrectable */, 0, 0);
	FTL_TEST_EXPECT_ASSERT(handle_nvme_io_cmd(&cmd));
}

/* ------------------------------------------------- completion helpers --- */

static void test_slot_release_writes_release_type_to_cpl_fifo(void)
{
	NVME_CPL_FIFO_REG cpl;
	int found;

	set_nvme_slot_release(0x33);

	TEST_ASSERT_EQUAL_size_t(1, mock_io_write_count_for(NVME_CPL_FIFO_REG_ADDR));
	TEST_ASSERT_EQUAL_size_t(0, mock_io_write_count_for(CPL_FIFO_DWORD1_ADDR));
	TEST_ASSERT_EQUAL_size_t(1, mock_io_write_count_for(CPL_FIFO_DWORD2_ADDR));
	cpl.dword[2] = mock_io_last_write(CPL_FIFO_DWORD2_ADDR, &found);
	TEST_ASSERT_TRUE(found);
	TEST_ASSERT_EQUAL_UINT(0x33, cpl.cmdSlotTag);
	TEST_ASSERT_EQUAL_UINT(CMD_SLOT_RELEASE_TYPE, cpl.cplType);
}

static void test_auto_completion_encodes_status_field(void)
{
	NVME_CPL_FIFO_REG cpl;
	int found;

	set_auto_nvme_cpl(0x12, 0xDEADBEEF, 0x4002);

	cpl.dword[1] = mock_io_last_write(CPL_FIFO_DWORD1_ADDR, &found);
	TEST_ASSERT_TRUE(found);
	cpl.dword[2] = mock_io_last_write(CPL_FIFO_DWORD2_ADDR, &found);
	TEST_ASSERT_TRUE(found);
	TEST_ASSERT_EQUAL_HEX32(0xDEADBEEF, cpl.specific);
	TEST_ASSERT_EQUAL_UINT(0x12, cpl.cmdSlotTag);
	TEST_ASSERT_EQUAL_HEX16(0x4002, cpl.statusFieldWord);
	TEST_ASSERT_EQUAL_UINT(AUTO_CPL_TYPE, cpl.cplType);
}

int main(void)
{
	UNITY_BEGIN();
	RUN_TEST(test_read_single_slice_decodes_lba_and_tag);
	RUN_TEST(test_read_two_slices_decodes_into_two_requests);
	RUN_TEST(test_read_unaligned_lba_splits_at_slice_boundary);
	RUN_TEST(test_read_sub_slice_range_produces_single_partial_request);
	RUN_TEST(test_read_with_trailing_partial_slice_covers_all_blocks);
	RUN_TEST(test_read_spanning_three_slices_with_partial_ends);
	RUN_TEST(test_read_of_highest_lba_is_accepted);
	RUN_TEST(test_read_does_not_post_completion);
	RUN_TEST(test_write_single_slice_decodes_lba_and_tag);
	RUN_TEST(test_write_three_slices_decodes_into_three_requests);
	RUN_TEST(test_write_ignores_fua_bit_in_dword12);
	RUN_TEST(test_flush_posts_auto_completion_with_success_status);
	RUN_TEST(test_flush_completion_carries_command_slot_tag);
	RUN_TEST(test_read_beyond_capacity_asserts);
	RUN_TEST(test_write_beyond_capacity_asserts);
	RUN_TEST(test_read_with_nonzero_upper_lba_dword_asserts);
	RUN_TEST(test_write_with_nonzero_upper_lba_dword_asserts);
	RUN_TEST(test_read_with_misaligned_prp1_asserts);
	RUN_TEST(test_read_with_misaligned_prp2_asserts);
	RUN_TEST(test_write_with_misaligned_prp1_asserts);
	RUN_TEST(test_write_with_misaligned_prp2_asserts);
	RUN_TEST(test_read_with_prp_upper_address_out_of_range_asserts);
	RUN_TEST(test_write_with_prp_upper_address_out_of_range_asserts);
	RUN_TEST(test_prp_upper_address_below_limit_is_accepted);
	RUN_TEST(test_unsupported_opcode_asserts_without_completion);
	RUN_TEST(test_write_uncorrectable_opcode_is_unsupported);
	RUN_TEST(test_slot_release_writes_release_type_to_cpl_fifo);
	RUN_TEST(test_auto_completion_encodes_status_field);
	return UNITY_END();
}
