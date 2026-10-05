#include "unity.h"

#include <string.h>

#include "ftl_fixture.h"
#include "address_translation.h"
#include "ftl_config.h"
#include "host_dram.h"
#include "memory_map.h"
#include "request_schedule.h"
#include "request_transform.h"

static unsigned int expected_row(unsigned int vsa)
{
    unsigned int die = Vsa2VdieTranslation(vsa);
    unsigned int block = Vsa2VblockTranslation(vsa);
    unsigned int phy = phyBlockMapPtr->phyBlock[die][Vblock2PblockOfTbsTranslation(block)].remappedPhyBlock;
    unsigned int lun = phy / TOTAL_BLOCKS_PER_LUN;
    unsigned int page = Vpage2PlsbPageTranslation(Vsa2VpageTranslation(vsa));
    return (lun ? LUN_1_BASE_ADDR : LUN_0_BASE_ADDR) +
           (phy % TOTAL_BLOCKS_PER_LUN) * PAGES_PER_MLC_BLOCK + page;
}

void setUp(void) {}
void tearDown(void) {}

void test_translation_roundtrips_and_overwrite(void)
{
    const unsigned int lsas[] = {0, 1, 12345, SLICES_PER_SSD - 1};
    unsigned int i;
    ftl_fixture_init();

    TEST_ASSERT_EQUAL_UINT(VSA_FAIL, AddrTransRead(500));
    for (i = 0; i < sizeof(lsas) / sizeof(lsas[0]); ++i) {
        unsigned int lsa = lsas[i];
        unsigned int vsa = AddrTransWrite(lsa);
        unsigned int die = Vsa2VdieTranslation(vsa);
        unsigned int block = Vsa2VblockTranslation(vsa);
        unsigned int page = Vsa2VpageTranslation(vsa);
        unsigned int phy = Vblock2PblockOfTbsTranslation(block);
        unsigned int remapped_phy = phyBlockMapPtr->phyBlock[die][phy].remappedPhyBlock;
        TEST_ASSERT_EQUAL_UINT(vsa, AddrTransRead(lsa));
        TEST_ASSERT_EQUAL_UINT(lsa, virtualSliceMapPtr->virtualSlice[vsa].logicalSliceAddr);
        TEST_ASSERT_EQUAL_UINT(vsa, Vorg2VsaTranslation(die, block, page));
        TEST_ASSERT_EQUAL_UINT(die, Pcw2VdieTranslation(Vdie2PchTranslation(die), Vdie2PwayTranslation(die)));
        TEST_ASSERT_EQUAL_UINT(BLOCK_STATE_NORMAL, phyBlockMapPtr->phyBlock[die][remapped_phy].bad);
        for (page = 0; page < USER_PAGES_PER_BLOCK; ++page)
            TEST_ASSERT_EQUAL_UINT(page, PlsbPage2VpageTranslation(Vpage2PlsbPageTranslation(page)));
    }

    {
        unsigned int old_vsa = AddrTransRead(lsas[1]);
        unsigned int die = Vsa2VdieTranslation(old_vsa);
        unsigned int block = Vsa2VblockTranslation(old_vsa);
        unsigned int invalid_before = virtualBlockMapPtr->block[die][block].invalidSliceCnt;
        unsigned int replacement = AddrTransWrite(lsas[1]);
        TEST_ASSERT_NOT_EQUAL(old_vsa, replacement);
        TEST_ASSERT_EQUAL_UINT(replacement, AddrTransRead(lsas[1]));
        TEST_ASSERT_EQUAL_UINT(invalid_before + 1, virtualBlockMapPtr->block[die][block].invalidSliceCnt);
        TEST_ASSERT_EQUAL_UINT(lsas[1], virtualSliceMapPtr->virtualSlice[old_vsa].logicalSliceAddr);
        InvalidateOldVsa(500);
        TEST_ASSERT_EQUAL_UINT(invalid_before + 1, virtualBlockMapPtr->block[die][block].invalidSliceCnt);
        logicalSliceMapPtr->logicalSlice[lsas[1]].virtualSliceAddr = old_vsa;
        virtualSliceMapPtr->virtualSlice[old_vsa].logicalSliceAddr = 500;
        InvalidateOldVsa(lsas[1]);
        TEST_ASSERT_EQUAL_UINT(invalid_before + 1, virtualBlockMapPtr->block[die][block].invalidSliceCnt);
        virtualSliceMapPtr->virtualSlice[old_vsa].logicalSliceAddr = lsas[1];
        logicalSliceMapPtr->logicalSlice[lsas[1]].virtualSliceAddr = replacement;
        TEST_ASSERT_EQUAL_UINT(replacement, AddrTransRead(lsas[1]));
    }
}

void test_slice_allocation_tracks_die_page_and_block_rollover(void)
{
    unsigned int i;
    ftl_fixture_init();
    for (i = 0; i < USER_DIES * 2; ++i) {
        unsigned int die = sliceAllocationTargetDie;
        unsigned int block = virtualDieMapPtr->die[die].currentBlock;
        unsigned int page = virtualBlockMapPtr->block[die][block].currentPage;
        unsigned int vsa = FindFreeVirtualSlice();
        TEST_ASSERT_EQUAL_UINT(die, Vsa2VdieTranslation(vsa));
        TEST_ASSERT_EQUAL_UINT(block, Vsa2VblockTranslation(vsa));
        TEST_ASSERT_EQUAL_UINT(page, Vsa2VpageTranslation(vsa));
        TEST_ASSERT_EQUAL_UINT(page + 1, virtualBlockMapPtr->block[die][block].currentPage);
    }

    {
        unsigned int die = sliceAllocationTargetDie;
        unsigned int old_block = virtualDieMapPtr->die[die].currentBlock;
        unsigned int free_before = virtualDieMapPtr->die[die].freeBlockCnt;
        unsigned int vsa;
        virtualBlockMapPtr->block[die][old_block].currentPage = USER_PAGES_PER_BLOCK;
        vsa = FindFreeVirtualSlice();
        TEST_ASSERT_NOT_EQUAL(old_block, Vsa2VblockTranslation(vsa));
        TEST_ASSERT_EQUAL_UINT(free_before - 1, virtualDieMapPtr->die[die].freeBlockCnt);
        TEST_ASSERT_EQUAL_UINT(0, virtualBlockMapPtr->block[die][Vsa2VblockTranslation(vsa)].free);
    }
}

void test_write_uses_real_nand_request_path(void)
{
    unsigned int lsa = 713;
    unsigned int entry = AVAILABLE_DATA_BUFFER_ENTRY_COUNT - 1;
    unsigned int origin;
    unsigned int vsa;
    unsigned int row;
    unsigned int die;
    unsigned int channel;
    unsigned int way;
    unsigned char *buffer;
    unsigned char *stored;
    unsigned int i;
    ftl_fixture_init();

    origin = GetFromFreeReqQ();
    reqPoolPtr->reqPool[origin].nvmeCmdSlotTag = 3;
    dataBufMapPtr->dataBuf[entry].logicalSliceAddr = lsa;
    dataBufMapPtr->dataBuf[entry].dirty = DATA_BUF_DIRTY;
    reqPoolPtr->reqPool[origin].dataBufInfo.entry = entry;
    buffer = (unsigned char *)host_dram_resolve((void *)(uintptr_t)
             ((unsigned int)DATA_BUFFER_BASE_ADDR + entry * BYTES_PER_DATA_REGION_OF_SLICE));
    for (i = 0; i < BYTES_PER_DATA_REGION_OF_SLICE; ++i)
        buffer[i] = (unsigned char)(i * 17U + 31U);

    fake_nand_stats_reset();
    EvictDataBufEntry(origin);
    SyncAllLowLevelReqDone();
    vsa = AddrTransRead(lsa);
    die = Vsa2VdieTranslation(vsa);
    channel = Vdie2PchTranslation(die);
    way = Vdie2PwayTranslation(die);
    row = expected_row(vsa);
    TEST_ASSERT_EQUAL_UINT(1, fake_nand_get_stats()->program);
    TEST_ASSERT_TRUE(fake_nand_is_programmed(channel, way, row));
    stored = fake_nand_page_ptr(channel, way, row);
    TEST_ASSERT_EQUAL_MEMORY(buffer, stored, BYTES_PER_DATA_REGION_OF_PAGE);
    TEST_ASSERT_EQUAL_UINT(DATA_BUF_CLEAN, dataBufMapPtr->dataBuf[entry].dirty);
}

void test_factory_bad_blocks_remap_and_bbt_persists(void)
{
    const unsigned int bad_block = 17;
    const unsigned int second_bad_block = 23;
    unsigned int die0 = Pcw2VdieTranslation(0, 0);
    unsigned int die1 = Pcw2VdieTranslation(0, 1);

    fake_nand_reset();
    fake_nand_mark_factory_bad(0, 0, bad_block);
    fake_nand_mark_factory_bad(0, 1, second_bad_block);
    ftl_fixture_init();

    TEST_ASSERT_EQUAL_UINT(BLOCK_STATE_BAD, phyBlockMapPtr->phyBlock[die0][bad_block].bad);
    TEST_ASSERT_EQUAL_UINT(BLOCK_STATE_BAD, phyBlockMapPtr->phyBlock[die1][second_bad_block].bad);
    TEST_ASSERT_TRUE(phyBlockMapPtr->phyBlock[die0][bad_block].remappedPhyBlock >= USER_BLOCKS_PER_LUN);
    TEST_ASSERT_TRUE(phyBlockMapPtr->phyBlock[die1][second_bad_block].remappedPhyBlock >= USER_BLOCKS_PER_LUN);
    TEST_ASSERT_EQUAL_UINT(BLOCK_STATE_NORMAL, phyBlockMapPtr->phyBlock[die0][18].bad);
    TEST_ASSERT_EQUAL_UINT(18, phyBlockMapPtr->phyBlock[die0][18].remappedPhyBlock);

    ftl_fixture_init();
    TEST_ASSERT_EQUAL_UINT(0, fake_nand_get_stats()->read_raw);
    TEST_ASSERT_EQUAL_UINT(BLOCK_STATE_BAD, phyBlockMapPtr->phyBlock[die0][bad_block].bad);
}

void test_failed_program_books_grown_bad_block_update(void)
{
    unsigned int lsa = 1203;
    unsigned int entry = AVAILABLE_DATA_BUFFER_ENTRY_COUNT - 1;
    unsigned int origin;
    unsigned int target_die;
    unsigned int target_block;
    unsigned int phy_block;
    ftl_fixture_init();
    target_die = sliceAllocationTargetDie;

    origin = GetFromFreeReqQ();
    reqPoolPtr->reqPool[origin].nvmeCmdSlotTag = 4;
    reqPoolPtr->reqPool[origin].dataBufInfo.entry = entry;
    dataBufMapPtr->dataBuf[entry].logicalSliceAddr = lsa;
    dataBufMapPtr->dataBuf[entry].dirty = DATA_BUF_DIRTY;
    fake_nand_fail_next(Vdie2PchTranslation(target_die), Vdie2PwayTranslation(target_die), FAKE_NAND_PROGRAM);
    EvictDataBufEntry(origin);
    SyncAllLowLevelReqDone();

    target_block = Vsa2VblockTranslation(AddrTransRead(lsa));
    phy_block = Vblock2PblockOfTbsTranslation(target_block);
    TEST_ASSERT_EQUAL_UINT(BLOCK_STATE_BAD, phyBlockMapPtr->phyBlock[target_die][phy_block].bad);
    TEST_ASSERT_EQUAL_UINT(BBT_INFO_GROWN_BAD_UPDATE_BOOKED,
                           bbtInfoMapPtr->bbtInfo[target_die].grownBadUpdate);
}

void test_read_trigger_failure_during_bad_block_scan_is_retried(void)
{
    static unsigned char clean_bad[TOTAL_BLOCKS_PER_DIE];
    const unsigned int bad_block = 17;
    unsigned int die0 = Pcw2VdieTranslation(0, 0);
    unsigned long clean_triggers;
    unsigned long clean_raw;
    unsigned int block;

    fake_nand_reset();
    fake_nand_mark_factory_bad(0, 0, bad_block);
    ftl_fixture_init();
    clean_triggers = fake_nand_get_stats()->read_trigger;
    clean_raw = fake_nand_get_stats()->read_raw;
    for (block = 0; block < TOTAL_BLOCKS_PER_DIE; ++block)
        clean_bad[block] = phyBlockMapPtr->phyBlock[die0][block].bad;

    fake_nand_reset();
    fake_nand_mark_factory_bad(0, 0, bad_block);
    fake_nand_fail_next(0, 0, FAKE_NAND_READ);
    ftl_fixture_init();

    TEST_ASSERT_EQUAL_UINT(clean_triggers + 1, fake_nand_get_stats()->read_trigger);
    TEST_ASSERT_EQUAL_UINT(clean_raw, fake_nand_get_stats()->read_raw);
    for (block = 0; block < TOTAL_BLOCKS_PER_DIE; ++block)
        TEST_ASSERT_EQUAL_UINT8_MESSAGE(clean_bad[block], phyBlockMapPtr->phyBlock[die0][block].bad,
                                        "retried read changed bad-block classification");
    TEST_ASSERT_EQUAL_UINT(BLOCK_STATE_BAD, phyBlockMapPtr->phyBlock[die0][bad_block].bad);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_translation_roundtrips_and_overwrite);
    RUN_TEST(test_slice_allocation_tracks_die_page_and_block_rollover);
    RUN_TEST(test_write_uses_real_nand_request_path);
    RUN_TEST(test_factory_bad_blocks_remap_and_bbt_persists);
    RUN_TEST(test_failed_program_books_grown_bad_block_update);
    RUN_TEST(test_read_trigger_failure_during_bad_block_scan_is_retried);
    return UNITY_END();
}
