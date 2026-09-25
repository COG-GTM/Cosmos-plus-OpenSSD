/* Unit tests for bad block management in address_translation.c: bad block
 * table read/recover/save, factory bad block scan, remapping to reserved
 * blocks and grown bad block bookkeeping. NAND contents are scripted through
 * mock_nsc read-fill and per-call hooks. */
#include "unity.h"

#include <string.h>

#include "fw_test.h"
#include "memory_map.h"
#include "xil_printf.h"

/* File-local firmware functions without a prototype in address_translation.h. */
void RemapBadBlock(void);
void InitDieMap(void);
void InitBlockMap(void);
void InitCurrentBlockOfDieMap(void);
void ReadBadBlockTable(unsigned int tempBbtBufAddr[], unsigned int tempBbtBufEntrySize);
void FindBadBlock(unsigned char dieState[], unsigned int tempBbtBufAddr[],
		unsigned int tempBbtBufEntrySize, unsigned int tempReadBufAddr[],
		unsigned int tempReadBufEntrySize);
void SaveBadBlockTable(unsigned char dieState[], unsigned int tempBbtBufAddr[],
		unsigned int tempBbtBufEntrySize);
void RecoverBadBlockTable(unsigned int tempBufAddr);

static const mock_nsc_call_t *last_call_of_cmd(unsigned int cmd)
{
	unsigned int i = mock_nsc_call_count();

	while (i > 0) {
		const mock_nsc_call_t *call = mock_nsc_call_at(--i);

		if (call != NULL && call->cmd == cmd)
			return call;
	}
	return NULL;
}

/* Row of the first bad block table page: bbt block 0, lsb page 1. */
#define BBT_FIRST_ROW (Vpage2PlsbPageTranslation(START_PAGE_NO_OF_BAD_BLOCK_TABLE_BLOCK))
#define BBT_ENTRY_SIZE (BYTES_PER_DATA_REGION_OF_PAGE + BYTES_PER_SPARE_REGION_OF_PAGE)
#define ROW_TO_PHY_BLOCK(row) (((row) % LUN_1_BASE_ADDR) / PAGES_PER_MLC_BLOCK + ((row) / LUN_1_BASE_ADDR) * TOTAL_BLOCKS_PER_LUN)
#define ROW_TO_PAGE(row) ((row) % PAGES_PER_MLC_BLOCK)

/* Scripted NAND image consumed by the hooks below. */
static struct {
	unsigned int lastTriggerRow[USER_CHANNELS][USER_WAYS];
	unsigned int bbtExistsDie;      /* die whose bbt page reads back as a valid table */
	unsigned int markPage0Die, markPage0Block;   /* factory mark in first row, spare byte */
	unsigned int markPage1Die, markPage1Block;   /* factory mark in last row, data byte */
} nand;

static void nand_hook(const mock_nsc_call_t *call)
{
	unsigned int dieNo;
	unsigned int row;
	unsigned char *buf = (unsigned char *)call->pageDataBuffer;

	if (call->channel == MOCK_NSC_CHANNEL_UNKNOWN)
		return;
	dieNo = Pcw2VdieTranslation(call->channel, call->way);

	switch (call->cmd) {
	case V2FCommand_ReadPageTrigger:
		nand.lastTriggerRow[call->channel][call->way] = call->rowAddress;
		break;
	case V2FCommand_ReadPageTransfer:
		/* ECC-on read: bad block table page */
		if (call->rowAddress == BBT_FIRST_ROW && dieNo == nand.bbtExistsDie)
			memset(buf, BLOCK_STATE_NORMAL, BYTES_PER_DATA_REGION_OF_PAGE);
		break;
	case V2FCommand_ReadPageTransferRaw:
		/* ECC-off read: factory bad block mark scan */
		row = nand.lastTriggerRow[call->channel][call->way];
		if (dieNo == nand.markPage0Die && ROW_TO_PHY_BLOCK(row) == nand.markPage0Block
				&& ROW_TO_PAGE(row) == BAD_BLOCK_MARK_PAGE0)
			buf[BAD_BLOCK_MARK_BYTE1] = 0x00;
		if (dieNo == nand.markPage1Die && ROW_TO_PHY_BLOCK(row) == nand.markPage1Block
				&& ROW_TO_PAGE(row) == BAD_BLOCK_MARK_PAGE1)
			buf[BAD_BLOCK_MARK_BYTE0] = 0x00;
		break;
	default:
		break;
	}
}

void setUp(void)
{
	fw_test_reset();
	fw_test_init_ftl();
	memset(&nand, 0xff, sizeof(nand));
}

void tearDown(void) {}

static void reset_phy_block_map(void)
{
	unsigned int dieNo, blockNo;

	for (dieNo = 0; dieNo < USER_DIES; dieNo++)
		for (blockNo = 0; blockNo < TOTAL_BLOCKS_PER_DIE; blockNo++) {
			phyBlockMapPtr->phyBlock[dieNo][blockNo].bad = 0;
			phyBlockMapPtr->phyBlock[dieNo][blockNo].remappedPhyBlock = blockNo;
		}
}

static unsigned char *bbt_entry(unsigned int dieNo, unsigned int phyBlockNo)
{
	return (unsigned char *)fw_ptr(RESERVED_DATA_BUFFER_BASE_ADDR
			+ dieNo * USED_PAGES_FOR_BAD_BLOCK_TABLE_PER_DIE * BBT_ENTRY_SIZE + phyBlockNo);
}

/* ---------------------------------------------------------------- init state */

static void test_init_marks_bbt_block_bad_and_remaps_it(void)
{
	unsigned int dieNo;

	for (dieNo = 0; dieNo < USER_DIES; dieNo++) {
		unsigned int bbtBlock = bbtInfoMapPtr->bbtInfo[dieNo].phyBlock;

		TEST_ASSERT_EQUAL_UINT(0, bbtBlock);
		TEST_ASSERT_EQUAL_UINT(BLOCK_STATE_BAD, phyBlockMapPtr->phyBlock[dieNo][bbtBlock].bad);
		TEST_ASSERT_EQUAL_UINT(USER_BLOCKS_PER_LUN, phyBlockMapPtr->phyBlock[dieNo][bbtBlock].remappedPhyBlock);
		TEST_ASSERT_EQUAL_UINT(BBT_INFO_GROWN_BAD_UPDATE_NONE, bbtInfoMapPtr->bbtInfo[dieNo].grownBadUpdate);
		TEST_ASSERT_EQUAL_UINT(0, virtualBlockMapPtr->block[dieNo][0].bad);
	}
	TEST_ASSERT_EQUAL_UINT(0, mbPerbadBlockSpace);
}

static void test_init_on_blank_nand_scans_and_saves_bad_block_table(void)
{
	fw_test_reset();
	InitFTL();

	/* bbt read + 2 mark pages per block (page 1 read only when page 0 is clean) */
	TEST_ASSERT_EQUAL_UINT(USER_DIES * (USED_PAGES_FOR_BAD_BLOCK_TABLE_PER_DIE + 2 * TOTAL_BLOCKS_PER_DIE),
			mock_nsc_count_cmd(V2FCommand_ReadPageTrigger));
	TEST_ASSERT_EQUAL_UINT(USER_DIES * 2 * TOTAL_BLOCKS_PER_DIE, mock_nsc_count_cmd(V2FCommand_ReadPageTransferRaw));
	TEST_ASSERT_EQUAL_UINT(USER_DIES * USED_PAGES_FOR_BAD_BLOCK_TABLE_PER_DIE, mock_nsc_count_cmd(V2FCommand_ProgramPage));
	/* bbt block erase per die + user block space erase */
	TEST_ASSERT_EQUAL_UINT(USER_DIES + USER_DIES * USER_BLOCKS_PER_DIE, mock_nsc_count_cmd(V2FCommand_BlockErase));
}

static void test_init_with_x_key_erases_total_block_space_instead(void)
{
	fw_test_reset();
	stub_inbyte_set('X');
	InitFTL();

	TEST_ASSERT_EQUAL_UINT(USER_DIES * TOTAL_BLOCKS_PER_DIE + USER_DIES, mock_nsc_count_cmd(V2FCommand_BlockErase));
}

/* ---------------------------------------------------------------- recover */

static void test_recover_with_existing_clean_table_skips_scan(void)
{
	unsigned int dieNo, blockNo;

	mock_nsc_set_read_fill(BLOCK_STATE_NORMAL);
	reset_phy_block_map();
	bbtInfoMapPtr->bbtInfo[0].grownBadUpdate = BBT_INFO_GROWN_BAD_UPDATE_BOOKED;

	RecoverBadBlockTable(RESERVED_DATA_BUFFER_BASE_ADDR);

	TEST_ASSERT_EQUAL_UINT(USER_DIES * USED_PAGES_FOR_BAD_BLOCK_TABLE_PER_DIE, mock_nsc_count_cmd(V2FCommand_ReadPageTransfer));
	TEST_ASSERT_EQUAL_UINT(0, mock_nsc_count_cmd(V2FCommand_ReadPageTransferRaw));
	TEST_ASSERT_EQUAL_UINT(0, mock_nsc_count_cmd(V2FCommand_ProgramPage));
	TEST_ASSERT_EQUAL_UINT(0, mock_nsc_count_cmd(V2FCommand_BlockErase));
	TEST_ASSERT_EQUAL_UINT(BBT_INFO_GROWN_BAD_UPDATE_NONE, bbtInfoMapPtr->bbtInfo[0].grownBadUpdate);
	for (dieNo = 0; dieNo < USER_DIES; dieNo++)
		for (blockNo = 0; blockNo < TOTAL_BLOCKS_PER_DIE; blockNo++)
			TEST_ASSERT_EQUAL_UINT(BLOCK_STATE_NORMAL, phyBlockMapPtr->phyBlock[dieNo][blockNo].bad);
}

static void test_recover_loads_bad_entries_from_existing_table(void)
{
	mock_nsc_set_read_fill(BLOCK_STATE_BAD);
	reset_phy_block_map();

	RecoverBadBlockTable(RESERVED_DATA_BUFFER_BASE_ADDR);

	TEST_ASSERT_EQUAL_UINT(0, mock_nsc_count_cmd(V2FCommand_ReadPageTransferRaw));
	TEST_ASSERT_EQUAL_UINT(BLOCK_STATE_BAD, phyBlockMapPtr->phyBlock[0][0].bad);
	TEST_ASSERT_EQUAL_UINT(BLOCK_STATE_BAD, phyBlockMapPtr->phyBlock[USER_DIES - 1][TOTAL_BLOCKS_PER_DIE - 1].bad);
}

static void test_read_bad_block_table_addresses_bbt_block_lsb_pages(void)
{
	unsigned int bufs[USER_DIES];
	unsigned int dieNo;
	const mock_nsc_call_t *call;

	for (dieNo = 0; dieNo < USER_DIES; dieNo++)
		bufs[dieNo] = RESERVED_DATA_BUFFER_BASE_ADDR + dieNo * BBT_ENTRY_SIZE;

	ReadBadBlockTable(bufs, BBT_ENTRY_SIZE);

	TEST_ASSERT_EQUAL_UINT(USER_DIES, mock_nsc_count_cmd(V2FCommand_ReadPageTrigger));
	call = mock_nsc_call_at(0);
	TEST_ASSERT_NOT_NULL(call);
	TEST_ASSERT_EQUAL_UINT(V2FCommand_ReadPageTrigger, call->cmd);
	TEST_ASSERT_EQUAL_HEX32(BBT_FIRST_ROW, call->rowAddress);
	TEST_ASSERT_EQUAL_UINT(0, call->channel);
	TEST_ASSERT_EQUAL_INT(0, call->way);
}

static void test_recover_scans_only_dies_without_table_and_saves_them(void)
{
	unsigned int dieNo;
	unsigned int scannedDies = USER_DIES - 1;

	nand.bbtExistsDie = 1;
	mock_nsc_set_hook(nand_hook);
	reset_phy_block_map();

	RecoverBadBlockTable(RESERVED_DATA_BUFFER_BASE_ADDR);

	TEST_ASSERT_EQUAL_UINT(scannedDies * 2 * TOTAL_BLOCKS_PER_DIE, mock_nsc_count_cmd(V2FCommand_ReadPageTransferRaw));
	TEST_ASSERT_EQUAL_UINT(scannedDies, mock_nsc_count_cmd(V2FCommand_BlockErase));
	TEST_ASSERT_EQUAL_UINT(scannedDies * USED_PAGES_FOR_BAD_BLOCK_TABLE_PER_DIE, mock_nsc_count_cmd(V2FCommand_ProgramPage));
	for (dieNo = 0; dieNo < USER_DIES; dieNo++)
		TEST_ASSERT_EQUAL_UINT(BLOCK_STATE_NORMAL, phyBlockMapPtr->phyBlock[dieNo][3].bad);
}

static void test_scan_detects_factory_marks_on_either_mark_page(void)
{
	const unsigned int page0Die = 0, page0Block = 5;
	const unsigned int page1Die = USER_DIES - 1, page1Block = TOTAL_BLOCKS_PER_LUN + 9;
	unsigned int dieNo, blockNo;
	unsigned int expectedRaw = USER_DIES * 2 * TOTAL_BLOCKS_PER_DIE - 1; /* page 1 skipped for page-0 mark */

	nand.markPage0Die = page0Die;
	nand.markPage0Block = page0Block;
	nand.markPage1Die = page1Die;
	nand.markPage1Block = page1Block;
	mock_nsc_set_hook(nand_hook);
	reset_phy_block_map();

	RecoverBadBlockTable(RESERVED_DATA_BUFFER_BASE_ADDR);

	TEST_ASSERT_EQUAL_UINT(expectedRaw, mock_nsc_count_cmd(V2FCommand_ReadPageTransferRaw));
	for (dieNo = 0; dieNo < USER_DIES; dieNo++)
		for (blockNo = 0; blockNo < TOTAL_BLOCKS_PER_DIE; blockNo++) {
			unsigned int isBad = (dieNo == page0Die && blockNo == page0Block)
					|| (dieNo == page1Die && blockNo == page1Block);

			TEST_ASSERT_EQUAL_UINT(isBad, phyBlockMapPtr->phyBlock[dieNo][blockNo].bad);
			TEST_ASSERT_EQUAL_UINT8(isBad, *bbt_entry(dieNo, blockNo));
		}
}

static void test_scan_with_all_blocks_marked_flags_every_block(void)
{
	unsigned char dieState[USER_DIES];
	unsigned int bbtBufs[USER_DIES], readBufs[USER_DIES];
	unsigned int dieNo;
	unsigned int readBase = RESERVED_DATA_BUFFER_BASE_ADDR + USER_DIES * USED_PAGES_FOR_BAD_BLOCK_TABLE_PER_DIE * BBT_ENTRY_SIZE;

	for (dieNo = 0; dieNo < USER_DIES; dieNo++) {
		dieState[dieNo] = dieNo == 0 ? DIE_STATE_BAD_BLOCK_TABLE_NOT_EXIST : DIE_STATE_BAD_BLOCK_TABLE_EXIST;
		bbtBufs[dieNo] = RESERVED_DATA_BUFFER_BASE_ADDR + dieNo * USED_PAGES_FOR_BAD_BLOCK_TABLE_PER_DIE * BBT_ENTRY_SIZE;
		readBufs[dieNo] = readBase + dieNo * BYTES_PER_NAND_ROW;
	}
	mock_nsc_set_read_fill(0x00);
	reset_phy_block_map();

	FindBadBlock(dieState, bbtBufs, BBT_ENTRY_SIZE, readBufs, BYTES_PER_NAND_ROW);

	/* only die 0 is scanned, and page 1 is never read once page 0 carries a mark */
	TEST_ASSERT_EQUAL_UINT(TOTAL_BLOCKS_PER_DIE, mock_nsc_count_cmd(V2FCommand_ReadPageTransferRaw));
	TEST_ASSERT_EQUAL_UINT(BLOCK_STATE_BAD, phyBlockMapPtr->phyBlock[0][0].bad);
	TEST_ASSERT_EQUAL_UINT(BLOCK_STATE_BAD, phyBlockMapPtr->phyBlock[0][TOTAL_BLOCKS_PER_DIE - 1].bad);
	TEST_ASSERT_EQUAL_UINT8(BLOCK_STATE_BAD, *bbt_entry(0, 17));
	TEST_ASSERT_EQUAL_UINT(BLOCK_STATE_NORMAL, phyBlockMapPtr->phyBlock[1][0].bad);
}

/* ---------------------------------------------------------------- save */

static void test_save_writes_only_dies_needing_update(void)
{
	unsigned char dieState[USER_DIES];
	unsigned int bbtBufs[USER_DIES];
	unsigned int dieNo;
	const mock_nsc_call_t *erase;
	const mock_nsc_call_t *program;

	for (dieNo = 0; dieNo < USER_DIES; dieNo++) {
		dieState[dieNo] = DIE_STATE_BAD_BLOCK_TABLE_HOLD;
		bbtBufs[dieNo] = RESERVED_DATA_BUFFER_BASE_ADDR + dieNo * USED_PAGES_FOR_BAD_BLOCK_TABLE_PER_DIE * BBT_ENTRY_SIZE;
	}
	dieState[1] = DIE_STATE_BAD_BLOCK_TABLE_UPDATE;
	dieState[2] = DIE_STATE_BAD_BLOCK_TABLE_EXIST;

	SaveBadBlockTable(dieState, bbtBufs, BBT_ENTRY_SIZE);

	TEST_ASSERT_EQUAL_UINT(1, mock_nsc_count_cmd(V2FCommand_BlockErase));
	TEST_ASSERT_EQUAL_UINT(USED_PAGES_FOR_BAD_BLOCK_TABLE_PER_DIE, mock_nsc_count_cmd(V2FCommand_ProgramPage));

	erase = mock_nsc_call_at(0);
	TEST_ASSERT_NOT_NULL(erase);
	TEST_ASSERT_EQUAL_UINT(V2FCommand_BlockErase, erase->cmd);
	TEST_ASSERT_EQUAL_UINT(Vdie2PchTranslation(1), erase->channel);
	TEST_ASSERT_EQUAL_INT(Vdie2PwayTranslation(1), erase->way);
	TEST_ASSERT_EQUAL_HEX32(bbtInfoMapPtr->bbtInfo[1].phyBlock * PAGES_PER_MLC_BLOCK, erase->rowAddress);

	program = last_call_of_cmd(V2FCommand_ProgramPage);
	TEST_ASSERT_NOT_NULL(program);
	TEST_ASSERT_EQUAL_HEX32(bbtInfoMapPtr->bbtInfo[1].phyBlock * PAGES_PER_MLC_BLOCK + BBT_FIRST_ROW, program->rowAddress);
	TEST_ASSERT_EQUAL_PTR(fw_ptr(bbtBufs[1]), program->pageDataBuffer);
}

/* ---------------------------------------------------------------- grown bad */

static void test_grown_bad_block_books_table_update(void)
{
	UpdatePhyBlockMapForGrownBadBlock(2, 40);

	TEST_ASSERT_EQUAL_UINT(BLOCK_STATE_BAD, phyBlockMapPtr->phyBlock[2][40].bad);
	TEST_ASSERT_EQUAL_UINT(BBT_INFO_GROWN_BAD_UPDATE_BOOKED, bbtInfoMapPtr->bbtInfo[2].grownBadUpdate);
	TEST_ASSERT_EQUAL_UINT(BBT_INFO_GROWN_BAD_UPDATE_NONE, bbtInfoMapPtr->bbtInfo[1].grownBadUpdate);
}

static void test_grown_bad_table_update_rewrites_booked_dies_only(void)
{
	const unsigned int dieNo = 1;
	const unsigned int bbtBlock = bbtInfoMapPtr->bbtInfo[dieNo].phyBlock;
	const mock_nsc_call_t *erase;

	UpdatePhyBlockMapForGrownBadBlock(dieNo, 40);
	UpdatePhyBlockMapForGrownBadBlock(dieNo, TOTAL_BLOCKS_PER_LUN + 3);

	UpdateBadBlockTableForGrownBadBlock(RESERVED_DATA_BUFFER_BASE_ADDR);

	TEST_ASSERT_EQUAL_UINT(1, mock_nsc_count_cmd(V2FCommand_BlockErase));
	TEST_ASSERT_EQUAL_UINT(USED_PAGES_FOR_BAD_BLOCK_TABLE_PER_DIE, mock_nsc_count_cmd(V2FCommand_ProgramPage));
	erase = mock_nsc_call_at(0);
	TEST_ASSERT_NOT_NULL(erase);
	TEST_ASSERT_EQUAL_UINT(Vdie2PchTranslation(dieNo), erase->channel);
	TEST_ASSERT_EQUAL_INT(Vdie2PwayTranslation(dieNo), erase->way);

	TEST_ASSERT_EQUAL_UINT8(BLOCK_STATE_BAD, *bbt_entry(dieNo, 40));
	TEST_ASSERT_EQUAL_UINT8(BLOCK_STATE_BAD, *bbt_entry(dieNo, TOTAL_BLOCKS_PER_LUN + 3));
	TEST_ASSERT_EQUAL_UINT8(BLOCK_STATE_NORMAL, *bbt_entry(dieNo, 41));
	/* the bbt block itself is flagged bad in DRAM only, never in the saved table */
	TEST_ASSERT_EQUAL_UINT(BLOCK_STATE_BAD, phyBlockMapPtr->phyBlock[dieNo][bbtBlock].bad);
	TEST_ASSERT_EQUAL_UINT8(BLOCK_STATE_NORMAL, *bbt_entry(dieNo, bbtBlock));
	/* the booking is not cleared by the update itself */
	TEST_ASSERT_EQUAL_UINT(BBT_INFO_GROWN_BAD_UPDATE_BOOKED, bbtInfoMapPtr->bbtInfo[dieNo].grownBadUpdate);
}

static void test_grown_bad_table_update_without_booking_is_noop(void)
{
	UpdateBadBlockTableForGrownBadBlock(RESERVED_DATA_BUFFER_BASE_ADDR);

	TEST_ASSERT_EQUAL_UINT(0, mock_nsc_call_count());
}

/* ---------------------------------------------------------------- remap */

static void test_remap_moves_lun0_bad_block_to_first_reserved_block(void)
{
	reset_phy_block_map();
	phyBlockMapPtr->phyBlock[0][7].bad = 1;
	phyBlockMapPtr->phyBlock[0][8].bad = 1;

	RemapBadBlock();

	TEST_ASSERT_EQUAL_UINT(USER_BLOCKS_PER_LUN, phyBlockMapPtr->phyBlock[0][7].remappedPhyBlock);
	TEST_ASSERT_EQUAL_UINT(USER_BLOCKS_PER_LUN + 1, phyBlockMapPtr->phyBlock[0][8].remappedPhyBlock);
	TEST_ASSERT_EQUAL_UINT(9, phyBlockMapPtr->phyBlock[0][9].remappedPhyBlock);
	TEST_ASSERT_EQUAL_UINT(7, phyBlockMapPtr->phyBlock[1][7].remappedPhyBlock);
	TEST_ASSERT_EQUAL_UINT(0, mbPerbadBlockSpace);
}

static void test_remap_skips_bad_reserved_blocks(void)
{
	reset_phy_block_map();
	phyBlockMapPtr->phyBlock[1][0].bad = 1;
	phyBlockMapPtr->phyBlock[1][USER_BLOCKS_PER_LUN].bad = 1;
	phyBlockMapPtr->phyBlock[1][USER_BLOCKS_PER_LUN + 1].bad = 1;

	RemapBadBlock();

	TEST_ASSERT_EQUAL_UINT(USER_BLOCKS_PER_LUN + 2, phyBlockMapPtr->phyBlock[1][0].remappedPhyBlock);
}

static void test_remap_moves_lun1_bad_block_within_lun1(void)
{
	const unsigned int lun1Block = TOTAL_BLOCKS_PER_LUN + 3;
	const unsigned int lun1Reserved = TOTAL_BLOCKS_PER_LUN + USER_BLOCKS_PER_LUN;

	reset_phy_block_map();
	phyBlockMapPtr->phyBlock[0][lun1Block].bad = 1;
	phyBlockMapPtr->phyBlock[0][lun1Reserved].bad = 1;

	RemapBadBlock();

	TEST_ASSERT_EQUAL_UINT(lun1Reserved + 1, phyBlockMapPtr->phyBlock[0][lun1Block].remappedPhyBlock);
	TEST_ASSERT_EQUAL_UINT(0, mbPerbadBlockSpace);
}

static void test_remap_without_reserved_blocks_left_counts_unmapped_bad_blocks(void)
{
	unsigned int blockNo;
	unsigned int reservedPerLun = TOTAL_BLOCKS_PER_LUN - USER_BLOCKS_PER_LUN;

	reset_phy_block_map();
	/* die 0: exhaust LUN0 reserve with reservedPerLun bad blocks, plus 2 more */
	for (blockNo = 0; blockNo < reservedPerLun + 2; blockNo++)
		phyBlockMapPtr->phyBlock[0][blockNo].bad = 1;
	/* die 1: all LUN1 reserved blocks bad, plus one user block */
	for (blockNo = TOTAL_BLOCKS_PER_LUN + USER_BLOCKS_PER_LUN; blockNo < TOTAL_BLOCKS_PER_DIE; blockNo++)
		phyBlockMapPtr->phyBlock[1][blockNo].bad = 1;
	phyBlockMapPtr->phyBlock[1][TOTAL_BLOCKS_PER_LUN].bad = 1;
	phyBlockMapPtr->phyBlock[1][TOTAL_BLOCKS_PER_LUN + 1].bad = 1;

	RemapBadBlock();

	TEST_ASSERT_EQUAL_UINT(TOTAL_BLOCKS_PER_LUN - 1, phyBlockMapPtr->phyBlock[0][reservedPerLun - 1].remappedPhyBlock);
	TEST_ASSERT_EQUAL_UINT(reservedPerLun, phyBlockMapPtr->phyBlock[0][reservedPerLun].remappedPhyBlock);
	TEST_ASSERT_EQUAL_UINT(reservedPerLun + 1, phyBlockMapPtr->phyBlock[0][reservedPerLun + 1].remappedPhyBlock);
	TEST_ASSERT_EQUAL_UINT(TOTAL_BLOCKS_PER_LUN, phyBlockMapPtr->phyBlock[1][TOTAL_BLOCKS_PER_LUN].remappedPhyBlock);
	TEST_ASSERT_EQUAL_UINT(2 * USER_DIES * MB_PER_BLOCK, mbPerbadBlockSpace);
}

static void test_block_map_init_excludes_unmapped_bad_blocks_from_free_list(void)
{
	const unsigned int badVirtualBlock = 5;
	const unsigned int badLun1VirtualBlock = USER_BLOCKS_PER_LUN + 6;
	unsigned int dieNo;

	reset_phy_block_map();
	phyBlockMapPtr->phyBlock[2][badVirtualBlock].bad = 1;
	phyBlockMapPtr->phyBlock[2][Vblock2PblockOfTbsTranslation(badLun1VirtualBlock)].bad = 1;

	InitDieMap();
	InitBlockMap();

	TEST_ASSERT_EQUAL_UINT(1, virtualBlockMapPtr->block[2][badVirtualBlock].bad);
	TEST_ASSERT_EQUAL_UINT(1, virtualBlockMapPtr->block[2][badLun1VirtualBlock].bad);
	TEST_ASSERT_EQUAL_UINT(BLOCK_NONE, virtualBlockMapPtr->block[2][badVirtualBlock].prevBlock);
	TEST_ASSERT_EQUAL_UINT(BLOCK_NONE, virtualBlockMapPtr->block[2][badVirtualBlock].nextBlock);
	TEST_ASSERT_EQUAL_UINT(USER_BLOCKS_PER_DIE - 2, virtualDieMapPtr->die[2].freeBlockCnt);
	for (dieNo = 0; dieNo < USER_DIES; dieNo++)
		if (dieNo != 2)
			TEST_ASSERT_EQUAL_UINT(USER_BLOCKS_PER_DIE, virtualDieMapPtr->die[dieNo].freeBlockCnt);
	TEST_ASSERT_EQUAL_UINT(0, virtualBlockMapPtr->block[2][4].bad);
	TEST_ASSERT_EQUAL_UINT(1, virtualBlockMapPtr->block[2][4].free);
}

static void test_block_map_init_follows_remap_to_good_reserved_block(void)
{
	reset_phy_block_map();
	phyBlockMapPtr->phyBlock[0][5].bad = 1;
	RemapBadBlock();

	InitDieMap();
	InitBlockMap();

	TEST_ASSERT_EQUAL_UINT(0, virtualBlockMapPtr->block[0][5].bad);
	TEST_ASSERT_EQUAL_UINT(USER_BLOCKS_PER_DIE, virtualDieMapPtr->die[0].freeBlockCnt);
}

static void test_current_block_init_asserts_when_die_has_no_free_block(void)
{
	while (virtualDieMapPtr->die[0].freeBlockCnt > RESERVED_FREE_BLOCK_COUNT)
		GetFromFbList(0, GET_FREE_BLOCK_NORMAL);

	FW_EXPECT_ASSERT(InitCurrentBlockOfDieMap());
}

int main(void)
{
	UNITY_BEGIN();
	RUN_TEST(test_init_marks_bbt_block_bad_and_remaps_it);
	RUN_TEST(test_init_on_blank_nand_scans_and_saves_bad_block_table);
	RUN_TEST(test_init_with_x_key_erases_total_block_space_instead);
	RUN_TEST(test_recover_with_existing_clean_table_skips_scan);
	RUN_TEST(test_recover_loads_bad_entries_from_existing_table);
	RUN_TEST(test_read_bad_block_table_addresses_bbt_block_lsb_pages);
	RUN_TEST(test_recover_scans_only_dies_without_table_and_saves_them);
	RUN_TEST(test_scan_detects_factory_marks_on_either_mark_page);
	RUN_TEST(test_scan_with_all_blocks_marked_flags_every_block);
	RUN_TEST(test_save_writes_only_dies_needing_update);
	RUN_TEST(test_grown_bad_block_books_table_update);
	RUN_TEST(test_grown_bad_table_update_rewrites_booked_dies_only);
	RUN_TEST(test_grown_bad_table_update_without_booking_is_noop);
	RUN_TEST(test_remap_moves_lun0_bad_block_to_first_reserved_block);
	RUN_TEST(test_remap_skips_bad_reserved_blocks);
	RUN_TEST(test_remap_moves_lun1_bad_block_within_lun1);
	RUN_TEST(test_remap_without_reserved_blocks_left_counts_unmapped_bad_blocks);
	RUN_TEST(test_block_map_init_excludes_unmapped_bad_blocks_from_free_list);
	RUN_TEST(test_block_map_init_follows_remap_to_good_reserved_block);
	RUN_TEST(test_current_block_init_asserts_when_die_has_no_free_block);
	return UNITY_END();
}
