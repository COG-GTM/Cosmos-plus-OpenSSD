#include "unity.h"

#include "ftl_fixture.h"
#include "request_allocation.h"
#include "request_transform.h"

void setUp(void) {}
void tearDown(void) {}

void test_free_request_queue_exhaustion(void)
{
    unsigned char seen[AVAILABLE_OUNTSTANDING_REQ_COUNT] = {0};
    unsigned int i;
    InitReqPool();
    for (i = 0; i < AVAILABLE_OUNTSTANDING_REQ_COUNT; ++i) {
        unsigned int tag = GetFromFreeReqQ();
        TEST_ASSERT_TRUE(tag < AVAILABLE_OUNTSTANDING_REQ_COUNT);
        TEST_ASSERT_EQUAL_UINT(0, seen[tag]);
        seen[tag] = 1;
    }
    TEST_ASSERT_EQUAL_UINT(0, freeReqQ.reqCnt);
    TEST_ASSERT_EQUAL_UINT(REQ_SLOT_TAG_NONE, freeReqQ.headReq);
}

void test_free_queue_reuses_released_tags_in_fifo_order(void)
{
    unsigned int tags[AVAILABLE_OUNTSTANDING_REQ_COUNT];
    unsigned int i;
    InitReqPool();
    for (i = 0; i < AVAILABLE_OUNTSTANDING_REQ_COUNT; ++i)
        tags[i] = GetFromFreeReqQ();
    PutToFreeReqQ(tags[0]);
    PutToFreeReqQ(tags[1]);
    PutToFreeReqQ(tags[2]);
    TEST_ASSERT_EQUAL_UINT(tags[0], GetFromFreeReqQ());
    TEST_ASSERT_EQUAL_UINT(tags[1], GetFromFreeReqQ());
    TEST_ASSERT_EQUAL_UINT(tags[2], GetFromFreeReqQ());
}

void test_double_free_is_rejected(void)
{
    unsigned int tag;
    KNOWN_BUG("PutToFreeReqQ accepts a request tag that is already free");
    InitReqPool();
    tag = GetFromFreeReqQ();
    PutToFreeReqQ(tag);
    PutToFreeReqQ(tag);
    TEST_ASSERT_TRUE(freeReqQ.reqCnt <= AVAILABLE_OUNTSTANDING_REQ_COUNT);
}

void test_request_queue_fifo_and_selective_removal(void)
{
    unsigned int tags[10];
    unsigned int i;
    InitReqPool();
    InitDataBuf();
    for (i = 0; i < 10; ++i)
        tags[i] = GetFromFreeReqQ();

    PutToSliceReqQ(tags[0]);
    PutToSliceReqQ(tags[1]);
    TEST_ASSERT_EQUAL_UINT(tags[0], GetFromSliceReqQ());
    TEST_ASSERT_EQUAL_UINT(tags[1], GetFromSliceReqQ());

    PutToBlockedByBufDepReqQ(tags[2]);
    PutToBlockedByBufDepReqQ(tags[3]);
    SelectiveGetFromBlockedByBufDepReqQ(tags[2]);
    TEST_ASSERT_EQUAL_UINT(tags[3], blockedByBufDepReqQ.headReq);
    SelectiveGetFromBlockedByBufDepReqQ(tags[3]);
    TEST_ASSERT_EQUAL_UINT(0, blockedByBufDepReqQ.reqCnt);

    PutToBlockedByRowAddrDepReqQ(tags[4], 0, 0);
    PutToBlockedByRowAddrDepReqQ(tags[5], 0, 0);
    SelectiveGetFromBlockedByRowAddrDepReqQ(tags[4], 0, 0);
    TEST_ASSERT_EQUAL_UINT(tags[5], blockedByRowAddrDepReqQ[0][0].headReq);
    SelectiveGetFromBlockedByRowAddrDepReqQ(tags[5], 0, 0);

    PutToNvmeDmaReqQ(tags[6]);
    PutToNvmeDmaReqQ(tags[7]);
    TEST_ASSERT_EQUAL_UINT(tags[6], nvmeDmaReqQ.headReq);
    SelectiveGetFromNvmeDmaReqQ(tags[6]);
    TEST_ASSERT_EQUAL_UINT(tags[7], nvmeDmaReqQ.headReq);
    SelectiveGetFromNvmeDmaReqQ(tags[7]);

    PutToNandReqQ(tags[8], 0, 0);
    PutToNandReqQ(tags[9], 0, 0);
    TEST_ASSERT_EQUAL_UINT(tags[8], nandReqQ[0][0].headReq);
    GetFromNandReqQ(0, 0, 0, 0);
    TEST_ASSERT_EQUAL_UINT(tags[9], nandReqQ[0][0].headReq);
    GetFromNandReqQ(0, 0, 0, 0);
    TEST_ASSERT_EQUAL_UINT(0, nandReqQ[0][0].reqCnt);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_free_request_queue_exhaustion);
    RUN_TEST(test_free_queue_reuses_released_tags_in_fifo_order);
    RUN_TEST(test_double_free_is_rejected);
    RUN_TEST(test_request_queue_fifo_and_selective_removal);
    return UNITY_END();
}
