#include "unity.h"

#include "ftl_fixture.h"
#include "address_translation.h"
#include "ftl_config.h"
#include "garbage_collection.h"
#include "request_schedule.h"
#include "request_transform.h"

#define GC_TEST_ALLOCATIONS (USER_DIES * USER_PAGES_PER_BLOCK)

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
    unsigned int lsa;
    unsigned int old_vsa;
    unsigned int row;
    unsigned int new_vsa;
    unsigned char *source;
    unsigned char *destination;
    unsigned char expected[BYTES_PER_DATA_REGION_OF_PAGE];
    unsigned int i;
    const fake_nand_stats *stats;
    ftl_fixture_init();

    victim = populate_die_block(die, 5000, page_lsa);
    TEST_ASSERT_NOT_EQUAL(BLOCK_NONE, victim);
    lsa = page_lsa[0];
    TEST_ASSERT_NOT_EQUAL(LSA_NONE, lsa);
    old_vsa = AddrTransRead(lsa);
    for (i = 1; i < USER_PAGES_PER_BLOCK; ++i) {
        TEST_ASSERT_NOT_EQUAL(LSA_NONE, page_lsa[i]);
        AddrTransWrite(page_lsa[i]);
    }
    sync_programmed_page_counts();
    TEST_ASSERT_EQUAL_UINT(SLICES_PER_BLOCK - 1,
                           virtualBlockMapPtr->block[die][victim].invalidSliceCnt);

    row = gc_expected_row(old_vsa);
    source = fake_nand_page_ptr(0, Vdie2PwayTranslation(die), row);
    for (i = 0; i < BYTES_PER_DATA_REGION_OF_PAGE; ++i)
        source[i] = expected[i] = (unsigned char)(i * 29U + 7U);
    fake_nand_mark_programmed(0, Vdie2PwayTranslation(die), row);
    TEST_ASSERT_TRUE(fake_nand_is_programmed(0, Vdie2PwayTranslation(die), row));

    fake_nand_stats_reset();
    GarbageCollection(die);
    SyncAllLowLevelReqDone();
    stats = fake_nand_get_stats();
    TEST_ASSERT_EQUAL_UINT(1, stats->read_transfer);
    TEST_ASSERT_EQUAL_UINT(1, stats->program);
    TEST_ASSERT_EQUAL_UINT(1, stats->erase);

    new_vsa = AddrTransRead(lsa);
    TEST_ASSERT_NOT_EQUAL(old_vsa, new_vsa);
    TEST_ASSERT_NOT_EQUAL(victim, Vsa2VblockTranslation(new_vsa));
    destination = fake_nand_page_ptr(0, Vdie2PwayTranslation(die), gc_expected_row(new_vsa));
    TEST_ASSERT_EQUAL_MEMORY(expected, destination, BYTES_PER_DATA_REGION_OF_PAGE);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_victim_selection_prefers_most_invalid_then_fifo);
    RUN_TEST(test_fully_invalid_block_is_erased_without_copy_io);
    RUN_TEST(test_partially_valid_block_copies_and_preserves_data);
    return UNITY_END();
}
