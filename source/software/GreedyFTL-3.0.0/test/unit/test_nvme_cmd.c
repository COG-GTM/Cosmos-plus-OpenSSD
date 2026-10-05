#include "unity.h"

#include <string.h>

#include "ftl_fixture.h"
#include "fake_host_dma.h"
#include "host_reg_map.h"
#include "ftl_config.h"
#include "memory_map.h"
#include "nvme/host_lld.h"
#include "nvme/nvme.h"
#include "nvme/nvme_admin_cmd.h"
#include "nvme/nvme_io_cmd.h"

extern volatile NVME_CONTEXT g_nvmeTask;

#define CPL_SLOT_TAG(dword2) ((dword2) & 0x7fU)
#define CPL_TYPE(dword2) (((dword2) >> 14) & 0x3U)

static NVME_COMMAND make_cmd(unsigned short qID, unsigned short slot, unsigned int opc)
{
    NVME_COMMAND cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.qID = qID;
    cmd.cmdSlotTag = slot;
    cmd.cmdDword[0] = opc;
    return cmd;
}

static NVME_ADMIN_COMMAND *admin(NVME_COMMAND *cmd)
{
    return (NVME_ADMIN_COMMAND *)cmd->cmdDword;
}

static NVME_IO_COMMAND *io(NVME_COMMAND *cmd)
{
    return (NVME_IO_COMMAND *)cmd->cmdDword;
}

static uint32_t last_cpl_dword(unsigned int offset)
{
    uint32_t value = 0;
    TEST_ASSERT_TRUE(host_reg_last_write(NVME_CPL_FIFO_REG_ADDR + offset, &value));
    return value;
}

void setUp(void)
{
    host_reg_reset();
    fake_host_dma_install();
    memset((void *)&g_nvmeTask, 0, sizeof(g_nvmeTask));
}

void tearDown(void) {}

void test_set_features_number_of_queues_is_clamped(void)
{
    NVME_COMMAND cmd = make_cmd(0, 3, ADMIN_SET_FEATURES);
    admin(&cmd)->dword10 = NUMBER_OF_QUEUES;
    admin(&cmd)->dword11 = (20U << 16) | 20U;
    handle_nvme_admin_cmd(&cmd);
    TEST_ASSERT_EQUAL_HEX32(((MAX_NUM_OF_IO_CQ - 1) << 16) | (MAX_NUM_OF_IO_SQ - 1), last_cpl_dword(4));
    TEST_ASSERT_EQUAL_UINT(3, CPL_SLOT_TAG(last_cpl_dword(8)));
    TEST_ASSERT_EQUAL_UINT(AUTO_CPL_TYPE, CPL_TYPE(last_cpl_dword(8)));
}

void test_volatile_write_cache_set_then_get(void)
{
    NVME_COMMAND set = make_cmd(0, 1, ADMIN_SET_FEATURES);
    NVME_COMMAND get = make_cmd(0, 2, ADMIN_GET_FEATURES);
    admin(&set)->dword10 = VOLATILE_WRITE_CACHE;
    admin(&set)->dword11 = 1;
    handle_nvme_admin_cmd(&set);
    TEST_ASSERT_EQUAL_UINT(1, g_nvmeTask.cacheEn);

    admin(&get)->dword10 = VOLATILE_WRITE_CACHE;
    handle_nvme_admin_cmd(&get);
    TEST_ASSERT_EQUAL_UINT(1, last_cpl_dword(4));
    TEST_ASSERT_EQUAL_UINT(2, CPL_SLOT_TAG(last_cpl_dword(8)));
}

void test_create_and_delete_io_queues_update_context(void)
{
    NVME_COMMAND cq = make_cmd(0, 4, ADMIN_CREATE_IO_CQ);
    NVME_COMMAND sq = make_cmd(0, 5, ADMIN_CREATE_IO_SQ);
    NVME_COMMAND dsq = make_cmd(0, 6, ADMIN_DELETE_IO_SQ);
    NVME_COMMAND dcq = make_cmd(0, 7, ADMIN_DELETE_IO_CQ);

    admin(&cq)->PRP1[0] = 0x00400000;
    admin(&cq)->dword10 = (0x3fU << 16) | 2U;
    admin(&cq)->dword11 = (3U << 16) | 0x2U;
    handle_nvme_admin_cmd(&cq);
    TEST_ASSERT_EQUAL_UINT(1, g_nvmeTask.ioCqInfo[1].valid);
    TEST_ASSERT_EQUAL_UINT(0x3f, g_nvmeTask.ioCqInfo[1].qSzie);
    TEST_ASSERT_EQUAL_UINT(3, g_nvmeTask.ioCqInfo[1].irqVector);
    TEST_ASSERT_EQUAL_HEX32(0x00400000, g_nvmeTask.ioCqInfo[1].pcieBaseAddrL);

    admin(&sq)->PRP1[0] = 0x00500000;
    admin(&sq)->dword10 = (0x3fU << 16) | 2U;
    admin(&sq)->dword11 = (2U << 16) | 0x1U;
    handle_nvme_admin_cmd(&sq);
    TEST_ASSERT_EQUAL_UINT(1, g_nvmeTask.ioSqInfo[1].valid);
    TEST_ASSERT_EQUAL_UINT(2, g_nvmeTask.ioSqInfo[1].cqVector);
    TEST_ASSERT_EQUAL_HEX32(0x00500000, g_nvmeTask.ioSqInfo[1].pcieBaseAddrL);

    admin(&dsq)->dword10 = 2;
    handle_nvme_admin_cmd(&dsq);
    TEST_ASSERT_EQUAL_UINT(0, g_nvmeTask.ioSqInfo[1].valid);
    admin(&dcq)->dword10 = 2;
    handle_nvme_admin_cmd(&dcq);
    TEST_ASSERT_EQUAL_UINT(0, g_nvmeTask.ioCqInfo[1].valid);
    TEST_ASSERT_EQUAL_UINT(7, CPL_SLOT_TAG(last_cpl_dword(8)));
}

void test_delete_io_sq_rejects_admin_queue_id(void)
{
    NVME_ADMIN_QUEUE_STATUS before;
    NVME_COMMAND cmd = make_cmd(0, 8, ADMIN_DELETE_IO_SQ);
    KNOWN_BUG("handle_delete_io_sq/cq do not validate QID; QID 0 writes ioSqInfo[-1] (adminQueueInfo)");
    memset((void *)&g_nvmeTask.adminQueueInfo, 0xa5, sizeof(g_nvmeTask.adminQueueInfo));
    memcpy(&before, (const void *)&g_nvmeTask.adminQueueInfo, sizeof(before));
    admin(&cmd)->dword10 = 0;
    handle_nvme_admin_cmd(&cmd);
    TEST_ASSERT_EQUAL_MEMORY(&before, (const void *)&g_nvmeTask.adminQueueInfo, sizeof(before));
}

void test_identify_page_aligned_prp_issues_single_dma(void)
{
    const fake_host_dma_command *dma;
    NVME_COMMAND cmd = make_cmd(0, 9, ADMIN_IDENTIFY);
    admin(&cmd)->dword10 = 1;
    admin(&cmd)->PRP1[0] = 0x00600000;
    admin(&cmd)->PRP1[1] = 0x1;
    handle_nvme_admin_cmd(&cmd);
    TEST_ASSERT_EQUAL_UINT(1, fake_host_dma_log_count());
    dma = fake_host_dma_log_get(0);
    TEST_ASSERT_EQUAL_HEX32(ADMIN_CMD_DRAM_DATA_BUFFER, dma->devAddr);
    TEST_ASSERT_EQUAL_HEX32(0x00600000, dma->words[2]);
    TEST_ASSERT_EQUAL_HEX32(0x1, dma->words[1]);
    TEST_ASSERT_EQUAL_UINT(0x1000, dma->len);
}

void test_identify_split_prp_issues_two_dmas(void)
{
    const fake_host_dma_command *dma;
    NVME_COMMAND cmd = make_cmd(0, 10, ADMIN_IDENTIFY);
    admin(&cmd)->dword10 = 0;
    admin(&cmd)->PRP1[0] = 0x00600800;
    admin(&cmd)->PRP2[0] = 0x00700000;
    handle_nvme_admin_cmd(&cmd);
    TEST_ASSERT_EQUAL_UINT(2, fake_host_dma_log_count());
    dma = fake_host_dma_log_get(0);
    TEST_ASSERT_EQUAL_UINT(0x800, dma->len);
    dma = fake_host_dma_log_get(1);
    TEST_ASSERT_EQUAL_HEX32(ADMIN_CMD_DRAM_DATA_BUFFER + 0x800, dma->devAddr);
    TEST_ASSERT_EQUAL_HEX32(0x00700000, dma->words[2]);
    TEST_ASSERT_EQUAL_UINT(0x800, dma->len);
}

void test_identify_rejects_unaligned_second_prp(void)
{
    const fake_host_dma_command *dma;
    NVME_COMMAND cmd = make_cmd(0, 11, ADMIN_IDENTIFY);
    KNOWN_BUG("handle_identify checks (PRP2[1] & 0xFFF) - the upper dword - instead of PRP2[0] page alignment");
    admin(&cmd)->dword10 = 1;
    admin(&cmd)->PRP1[0] = 0x00600800;
    admin(&cmd)->PRP2[0] = 0x00700120;
    handle_nvme_admin_cmd(&cmd);
    TEST_ASSERT_EQUAL_UINT(2, fake_host_dma_log_count());
    dma = fake_host_dma_log_get(1);
    TEST_ASSERT_EQUAL_HEX32(0, dma->words[2] & 0xFFFU);
}

void test_get_log_page_and_async_event(void)
{
    NVME_COMMAND log = make_cmd(0, 12, ADMIN_GET_LOG_PAGE);
    NVME_COMMAND aer = make_cmd(0, 13, ADMIN_ASYNCHRONOUS_EVENT_REQUEST);
    handle_nvme_admin_cmd(&log);
    TEST_ASSERT_EQUAL_UINT(0x9, last_cpl_dword(4));
    handle_nvme_admin_cmd(&aer);
    TEST_ASSERT_EQUAL_UINT(13, CPL_SLOT_TAG(last_cpl_dword(8)));
    TEST_ASSERT_EQUAL_UINT(CMD_SLOT_RELEASE_TYPE, CPL_TYPE(last_cpl_dword(8)));
}

void test_io_flush_completes_immediately(void)
{
    NVME_COMMAND cmd = make_cmd(1, 14, IO_NVM_FLUSH);
    handle_nvme_io_cmd(&cmd);
    TEST_ASSERT_EQUAL_UINT(14, CPL_SLOT_TAG(last_cpl_dword(8)));
    TEST_ASSERT_EQUAL_UINT(AUTO_CPL_TYPE, CPL_TYPE(last_cpl_dword(8)));
}

void test_io_write_then_read_round_trip_through_ftl(void)
{
    size_t i, rx = 0, tx = 0;
    unsigned int startLba = 2 * NVME_BLOCKS_PER_SLICE + 1;
    NVME_COMMAND wr = make_cmd(1, 15, IO_NVM_WRITE);
    NVME_COMMAND rd = make_cmd(1, 16, IO_NVM_READ);

    ftl_fixture_init_fresh_nand();
    host_reg_reset();
    fake_host_dma_install();

    io(&wr)->dword[10] = startLba;
    io(&wr)->dword[12] = NVME_BLOCKS_PER_SLICE;
    handle_nvme_io_cmd(&wr);
    TEST_ASSERT_EQUAL_UINT(2, sliceReqQ.reqCnt);
    TEST_ASSERT_EQUAL_UINT(2, reqPoolPtr->reqPool[sliceReqQ.headReq].logicalSliceAddr);
    TEST_ASSERT_EQUAL_UINT(3, reqPoolPtr->reqPool[sliceReqQ.tailReq].logicalSliceAddr);
    ReqTransSliceToLowLevel();
    SyncAllLowLevelReqDone();

    io(&rd)->dword[10] = startLba;
    io(&rd)->dword[12] = NVME_BLOCKS_PER_SLICE;
    handle_nvme_io_cmd(&rd);
    ReqTransSliceToLowLevel();
    SyncAllLowLevelReqDone();

    for (i = 0; i < fake_host_dma_log_count(); ++i) {
        const fake_host_dma_command *dma = fake_host_dma_log_get(i);
        if (dma->type != HOST_DMA_AUTO_TYPE)
            continue;
        if (dma->direction == HOST_DMA_RX_DIRECTION)
            ++rx;
        else
            ++tx;
    }
    TEST_ASSERT_EQUAL_UINT(NVME_BLOCKS_PER_SLICE + 1, rx);
    TEST_ASSERT_EQUAL_UINT(NVME_BLOCKS_PER_SLICE + 1, tx);
    TEST_ASSERT_EQUAL_UINT(0, sliceReqQ.reqCnt);
    TEST_ASSERT_EQUAL_UINT(AVAILABLE_OUNTSTANDING_REQ_COUNT, freeReqQ.reqCnt);
}

void test_io_write_past_capacity_is_rejected(void)
{
    unsigned int i, tag;
    NVME_COMMAND cmd = make_cmd(1, 17, IO_NVM_WRITE);
    KNOWN_BUG("handle_nvme_io_write only checks startLba < capacity; startLba + NLB can address slices past SLICES_PER_SSD");
    ftl_fixture_init();
    io(&cmd)->dword[10] = storageCapacity_L - 1;
    io(&cmd)->dword[12] = 2 * NVME_BLOCKS_PER_SLICE;
    handle_nvme_io_cmd(&cmd);
    tag = sliceReqQ.headReq;
    for (i = 0; i < sliceReqQ.reqCnt; ++i) {
        TEST_ASSERT_TRUE(reqPoolPtr->reqPool[tag].logicalSliceAddr * NVME_BLOCKS_PER_SLICE < storageCapacity_L);
        tag = reqPoolPtr->reqPool[tag].nextReq;
    }
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_set_features_number_of_queues_is_clamped);
    RUN_TEST(test_volatile_write_cache_set_then_get);
    RUN_TEST(test_create_and_delete_io_queues_update_context);
    RUN_TEST(test_identify_page_aligned_prp_issues_single_dma);
    RUN_TEST(test_identify_split_prp_issues_two_dmas);
    RUN_TEST(test_identify_rejects_unaligned_second_prp);
    RUN_TEST(test_get_log_page_and_async_event);
    RUN_TEST(test_io_flush_completes_immediately);
    RUN_TEST(test_io_write_then_read_round_trip_through_ftl);
    RUN_TEST(test_io_write_past_capacity_is_rejected);
    RUN_TEST(test_delete_io_sq_rejects_admin_queue_id);
    return UNITY_END();
}
