#include "unity.h"

#include <stdint.h>

#include "ftl_fixture.h"
#include "address_translation.h"
#include "data_buffer.h"
#include "ftl_config.h"
#include "host_dram.h"
#include "memory_map.h"
#include "request_schedule.h"
#include "request_transform.h"

static unsigned int page_row(unsigned int vsa)
{
    unsigned int die = Vsa2VdieTranslation(vsa);
    unsigned int block = Vsa2VblockTranslation(vsa);
    unsigned int phy = phyBlockMapPtr->phyBlock[die][Vblock2PblockOfTbsTranslation(block)].remappedPhyBlock;
    return (phy / TOTAL_BLOCKS_PER_LUN ? LUN_1_BASE_ADDR : LUN_0_BASE_ADDR) +
           (phy % TOTAL_BLOCKS_PER_LUN) * PAGES_PER_MLC_BLOCK +
           Vpage2PlsbPageTranslation(Vsa2VpageTranslation(vsa));
}

void setUp(void) {}
void tearDown(void) {}

void test_data_buffer_hit_miss_and_lru_eviction(void)
{
    unsigned int req;
    unsigned int entry;
    unsigned int i;
    InitReqPool();
    InitDataBuf();

    req = GetFromFreeReqQ();
    reqPoolPtr->reqPool[req].logicalSliceAddr = 75;
    TEST_ASSERT_EQUAL_UINT(DATA_BUF_FAIL, CheckDataBufHit(req));
    entry = AllocateDataBuf();
    TEST_ASSERT_EQUAL_UINT(AVAILABLE_DATA_BUFFER_ENTRY_COUNT - 1, entry);
    dataBufMapPtr->dataBuf[entry].logicalSliceAddr = 75;
    PutToDataBufHashList(entry);
    TEST_ASSERT_EQUAL_UINT(entry, CheckDataBufHit(req));
    TEST_ASSERT_EQUAL_UINT(entry, dataBufLruList.headEntry);
    TEST_ASSERT_EQUAL_UINT(AVAILABLE_DATA_BUFFER_ENTRY_COUNT - 2, AllocateDataBuf());
    for (i = 1; i < AVAILABLE_DATA_BUFFER_ENTRY_COUNT; ++i)
        (void)AllocateDataBuf();
    TEST_ASSERT_EQUAL_UINT(DATA_BUF_FAIL, CheckDataBufHit(req));
}

void test_dirty_eviction_writes_back_and_clean_eviction_does_not(void)
{
    unsigned int lsa = 1881;
    unsigned int entry = AVAILABLE_DATA_BUFFER_ENTRY_COUNT - 1;
    unsigned int origin;
    unsigned int vsa;
    unsigned int row;
    unsigned int die;
    unsigned int channel;
    unsigned int way;
    unsigned int i;
    unsigned char *buffer;
    unsigned char *stored;
    ftl_fixture_init();

    origin = GetFromFreeReqQ();
    reqPoolPtr->reqPool[origin].nvmeCmdSlotTag = 1;
    reqPoolPtr->reqPool[origin].dataBufInfo.entry = entry;
    dataBufMapPtr->dataBuf[entry].logicalSliceAddr = lsa;
    dataBufMapPtr->dataBuf[entry].dirty = DATA_BUF_DIRTY;
    buffer = (unsigned char *)host_dram_resolve((void *)(uintptr_t)
             ((unsigned int)DATA_BUFFER_BASE_ADDR + entry * BYTES_PER_DATA_REGION_OF_SLICE));
    for (i = 0; i < BYTES_PER_DATA_REGION_OF_SLICE; ++i)
        buffer[i] = (unsigned char)(i * 11U + 3U);

    fake_nand_stats_reset();
    EvictDataBufEntry(origin);
    SyncAllLowLevelReqDone();
    TEST_ASSERT_EQUAL_UINT(1, fake_nand_get_stats()->program);
    TEST_ASSERT_EQUAL_UINT(DATA_BUF_CLEAN, dataBufMapPtr->dataBuf[entry].dirty);
    vsa = AddrTransRead(lsa);
    die = Vsa2VdieTranslation(vsa);
    channel = Vdie2PchTranslation(die);
    way = Vdie2PwayTranslation(die);
    row = page_row(vsa);
    TEST_ASSERT_TRUE(fake_nand_is_programmed(channel, way, row));
    stored = fake_nand_page_ptr(channel, way, row);
    TEST_ASSERT_EQUAL_MEMORY(buffer, stored, BYTES_PER_DATA_REGION_OF_PAGE);

    origin = GetFromFreeReqQ();
    reqPoolPtr->reqPool[origin].dataBufInfo.entry = 0;
    dataBufMapPtr->dataBuf[0].logicalSliceAddr = lsa + 1;
    dataBufMapPtr->dataBuf[0].dirty = DATA_BUF_CLEAN;
    fake_nand_stats_reset();
    EvictDataBufEntry(origin);
    SyncAllLowLevelReqDone();
    TEST_ASSERT_EQUAL_UINT(0, fake_nand_get_stats()->program);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_data_buffer_hit_miss_and_lru_eviction);
    RUN_TEST(test_dirty_eviction_writes_back_and_clean_eviction_does_not);
    return UNITY_END();
}
