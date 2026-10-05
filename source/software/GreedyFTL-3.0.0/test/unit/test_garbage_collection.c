#include "unity.h"

#include "ftl_fixture.h"
#include "address_translation.h"
#include "ftl_config.h"
#include "garbage_collection.h"
#include "request_schedule.h"
#include "request_transform.h"

#define GC_TEST_ALLOCATIONS (USER_DIES * USER_PAGES_PER_BLOCK)
#define GC_VALID_PAGE_COUNT 3U

static unsigned int gc_expected_row(unsigned int vsa)
{
    unsigned int die = Vsa2VdieTranslation(vsa);
    unsigned int block = Vsa2VblockTranslation(vsa);
    unsigned int phy = phyBlockMapPtr->phyBlock[die][Vblock2PblockOfTbsTranslation(block)].remappedPhyBlock;
    unsigned int lun = phy / TOTAL_BLOCKS_PER_LUN;
    unsigned int page = Vpage2PlsbPageTranslation(Vsa2VpageTranslation(vsa));
    return (lun ? LUN_1_BASE_ADDR : LUN_0_BASE_ADDR) +
           (phy % TOTAL_BLOCKS_PER_LUN) * PAGES_PER_MLC_BLOCK + page;
}

static unsigned int populate_die_block(unsigned int die, unsigned int lsa_base,
                                       unsigned int page_lsa[USER_PAGES_PER_BLOCK])
{
    unsigned int lsas[GC_TEST_ALLOCATIONS];
    unsigned int i;
    unsigned int block = BLOCK_NONE;

    for (i = 0; i < USER_PAGES_PER_BLOCK; ++i)
        page_lsa[i] = LSA_NONE;

    for (i = 0; i < GC_TEST_ALLOCATIONS; ++i) {
        unsigned int vsa;
        lsas[i] = lsa_base + i;
        vsa = AddrTransWrite(lsas[i]);
        if (Vsa2VdieTranslation(vsa) == die) {
            if (block == BLOCK_NONE)
                block = Vsa2VblockTranslation(vsa);
            if (Vsa2VblockTranslation(vsa) == block)
                page_lsa[Vsa2VpageTranslation(vsa)] = lsas[i];
        }
    }
    return block;
}

static void sync_programmed_page_counts(void)
{
    unsigned int die;
    unsigned int block;

    /* AddrTransWrite allocates VSAs without programming; row dependencies would otherwise block the GC erase. */
    for (die = 0; die < USER_DIES; ++die) {
        unsigned int channel = Vdie2PchTranslation(die);
        unsigned int way = Vdie2PwayTranslation(die);
        for (block = 0; block < MAIN_BLOCKS_PER_DIE; ++block)
            rowAddrDependencyTablePtr->block[channel][way][block].permittedProgPage =
                virtualBlockMapPtr->block[die][block].currentPage;
    }
}

void setUp(void) {}
void tearDown(void) {}

void test_victim_selection_prefers_most_invalid_then_fifo(void)
{
    unsigned int die = 0;
    unsigned int first = 100;
    unsigned int second = 101;
    unsigned int third = 102;
    ftl_fixture_init();

    virtualBlockMapPtr->block[die][first].invalidSliceCnt = 3;
    virtualBlockMapPtr->block[die][second].invalidSliceCnt = 7;
    virtualBlockMapPtr->block[die][third].invalidSliceCnt = 7;
    PutToGcVictimList(die, first, 3);
    PutToGcVictimList(die, second, 7);
    PutToGcVictimList(die, third, 7);
    TEST_ASSERT_EQUAL_UINT(second, GetFromGcVictimList(die));
    TEST_ASSERT_EQUAL_UINT(third, GetFromGcVictimList(die));
    TEST_ASSERT_EQUAL_UINT(first, GetFromGcVictimList(die));
}

void test_fully_invalid_block_is_erased_without_copy_io(void)
{
    unsigned int die = 0;
    unsigned int block;
    unsigned int free_before;
    unsigned int page_lsa[USER_PAGES_PER_BLOCK];
    unsigned int page;
    const fake_nand_stats *stats;
    ftl_fixture_init();

    block = populate_die_block(die, 1000, page_lsa);
    TEST_ASSERT_NOT_EQUAL(BLOCK_NONE, block);
    for (page = 0; page < USER_PAGES_PER_BLOCK; ++page) {
        TEST_ASSERT_NOT_EQUAL(LSA_NONE, page_lsa[page]);
        AddrTransWrite(page_lsa[page]);
    }
    sync_programmed_page_counts();
    TEST_ASSERT_EQUAL_UINT(SLICES_PER_BLOCK, virtualBlockMapPtr->block[die][block].invalidSliceCnt);
    free_before = virtualDieMapPtr->die[die].freeBlockCnt;
    fake_nand_stats_reset();
    GarbageCollection(die);
    SyncAllLowLevelReqDone();

    stats = fake_nand_get_stats();
    TEST_ASSERT_EQUAL_UINT(0, stats->read_trigger);
    TEST_ASSERT_EQUAL_UINT(0, stats->read_transfer);
    TEST_ASSERT_EQUAL_UINT(0, stats->read_raw);
    TEST_ASSERT_EQUAL_UINT(0, stats->program);
    TEST_ASSERT_EQUAL_UINT(1, stats->erase);
    TEST_ASSERT_EQUAL_UINT(free_before + 1, virtualDieMapPtr->die[die].freeBlockCnt);
    TEST_ASSERT_EQUAL_UINT(0, virtualBlockMapPtr->block[die][block].invalidSliceCnt);
    TEST_ASSERT_EQUAL_UINT(0, virtualBlockMapPtr->block[die][block].currentPage);
    TEST_ASSERT_EQUAL_UINT(1, virtualBlockMapPtr->block[die][block].free);
}

void test_partially_valid_block_copies_and_preserves_data(void)
{
    unsigned int die = 0;
    unsigned int victim;
    unsigned int page_lsa[USER_PAGES_PER_BLOCK];
    const unsigned int valid_pages[GC_VALID_PAGE_COUNT] = {0, 5, 9};
    unsigned int valid_lsa[GC_VALID_PAGE_COUNT];
    unsigned int old_vsa[GC_VALID_PAGE_COUNT];
    unsigned int new_vsa;
    unsigned int row;
    unsigned int page;
    unsigned char *source;
    unsigned char *destination;
    unsigned char expected[GC_VALID_PAGE_COUNT][BYTES_PER_DATA_REGION_OF_PAGE];
    unsigned int i;
    const fake_nand_stats *stats;
    ftl_fixture_init();

    victim = populate_die_block(die, 5000, page_lsa);
    TEST_ASSERT_NOT_EQUAL(BLOCK_NONE, victim);
    for (i = 0; i < GC_VALID_PAGE_COUNT; ++i) {
        valid_lsa[i] = page_lsa[valid_pages[i]];
        TEST_ASSERT_NOT_EQUAL(LSA_NONE, valid_lsa[i]);
        old_vsa[i] = AddrTransRead(valid_lsa[i]);
        TEST_ASSERT_EQUAL_UINT(victim, Vsa2VblockTranslation(old_vsa[i]));
    }
    for (page = 0; page < USER_PAGES_PER_BLOCK; ++page) {
        int keep_page = 0;
        for (i = 0; i < GC_VALID_PAGE_COUNT; ++i)
            if (page == valid_pages[i])
                keep_page = 1;
        if (!keep_page) {
            TEST_ASSERT_NOT_EQUAL(LSA_NONE, page_lsa[page]);
            AddrTransWrite(page_lsa[page]);
        }
    }
    sync_programmed_page_counts();
    TEST_ASSERT_EQUAL_UINT(SLICES_PER_BLOCK - GC_VALID_PAGE_COUNT,
                           virtualBlockMapPtr->block[die][victim].invalidSliceCnt);

    for (i = 0; i < GC_VALID_PAGE_COUNT; ++i) {
        unsigned int source_die = Vsa2VdieTranslation(old_vsa[i]);
        unsigned int source_channel = Vdie2PchTranslation(source_die);
        unsigned int source_way = Vdie2PwayTranslation(source_die);
        row = gc_expected_row(old_vsa[i]);
        source = fake_nand_page_ptr(source_channel, source_way, row);
        for (page = 0; page < BYTES_PER_DATA_REGION_OF_PAGE; ++page)
            source[page] = expected[i][page] = (unsigned char)(i * 37U + page * 29U + 7U);
        fake_nand_mark_programmed(source_channel, source_way, row);
        TEST_ASSERT_TRUE(fake_nand_is_programmed(source_channel, source_way, row));
    }

    fake_nand_stats_reset();
    GarbageCollection(die);
    SyncAllLowLevelReqDone();
    stats = fake_nand_get_stats();
    TEST_ASSERT_EQUAL_UINT(GC_VALID_PAGE_COUNT, stats->read_trigger);
    TEST_ASSERT_EQUAL_UINT(GC_VALID_PAGE_COUNT, stats->read_transfer);
    TEST_ASSERT_EQUAL_UINT(GC_VALID_PAGE_COUNT, stats->program);
    TEST_ASSERT_EQUAL_UINT(1, stats->erase);

    for (i = 0; i < GC_VALID_PAGE_COUNT; ++i) {
        unsigned int destination_die;
        unsigned int destination_channel;
        unsigned int destination_way;
        new_vsa = AddrTransRead(valid_lsa[i]);
        TEST_ASSERT_NOT_EQUAL(old_vsa[i], new_vsa);
        TEST_ASSERT_NOT_EQUAL(victim, Vsa2VblockTranslation(new_vsa));
        destination_die = Vsa2VdieTranslation(new_vsa);
        destination_channel = Vdie2PchTranslation(destination_die);
        destination_way = Vdie2PwayTranslation(destination_die);
        destination = fake_nand_page_ptr(destination_channel, destination_way,
                                         gc_expected_row(new_vsa));
        TEST_ASSERT_EQUAL_MEMORY(expected[i], destination, BYTES_PER_DATA_REGION_OF_PAGE);
    }
}

void test_zero_invalid_victim_bucket_is_skipped(void)
{
    unsigned int die = 0;
    unsigned int block = 100;
    unsigned int invalid_slice_cnt;

    KNOWN_BUG("GetFromGcVictimList skips the zero-invalid bucket and asserts when no other victim exists");
    ftl_fixture_init();
    for (invalid_slice_cnt = 1; invalid_slice_cnt <= SLICES_PER_BLOCK; ++invalid_slice_cnt)
        TEST_ASSERT_EQUAL_UINT(BLOCK_NONE,
                               gcVictimMapPtr->gcVictimList[die][invalid_slice_cnt].headBlock);
    virtualBlockMapPtr->block[die][block].invalidSliceCnt = 0;
    PutToGcVictimList(die, block, 0);
    TEST_ASSERT_EQUAL_UINT(block, GetFromGcVictimList(die));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_victim_selection_prefers_most_invalid_then_fifo);
    RUN_TEST(test_fully_invalid_block_is_erased_without_copy_io);
    RUN_TEST(test_partially_valid_block_copies_and_preserves_data);
    RUN_TEST(test_zero_invalid_victim_bucket_is_skipped);
    return UNITY_END();
}
