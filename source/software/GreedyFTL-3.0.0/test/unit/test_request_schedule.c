#include "unity.h"

#include "ftl_fixture.h"
#include "memory_map.h"
#include "request_schedule.h"

extern P_DIE_STATE_TABLE dieStateTablePtr;

void setUp(void)
{
    ftl_fixture_init();
}

void tearDown(void) {}

static void move_idle_way_to_status_check(unsigned int chNo, unsigned int wayNo)
{
    SelectivGetFromNandIdleList(chNo, wayNo);
    PutToNandStatusCheckList(chNo, wayNo);
}

void test_scheduler_init_puts_all_ways_on_idle_list(void)
{
    unsigned int chNo, wayNo, count;
    for (chNo = 0; chNo < USER_CHANNELS; ++chNo) {
        count = 0;
        for (wayNo = wayPriorityTablePtr->wayPriority[chNo].idleHead; wayNo != WAY_NONE;
             wayNo = dieStateTablePtr->dieState[chNo][wayNo].nextWay)
            ++count;
        TEST_ASSERT_EQUAL_UINT(USER_WAYS, count);
        TEST_ASSERT_EQUAL_UINT(WAY_NONE, wayPriorityTablePtr->wayPriority[chNo].statusCheckHead);
    }
}

void test_way_list_selective_removal_keeps_links(void)
{
    move_idle_way_to_status_check(0, 1);
    move_idle_way_to_status_check(0, 2);
    move_idle_way_to_status_check(0, 3);
    SelectiveGetFromNandStatusCheckList(0, 2);
    TEST_ASSERT_EQUAL_UINT(1, wayPriorityTablePtr->wayPriority[0].statusCheckHead);
    TEST_ASSERT_EQUAL_UINT(3, wayPriorityTablePtr->wayPriority[0].statusCheckTail);
    TEST_ASSERT_EQUAL_UINT(3, dieStateTablePtr->dieState[0][1].nextWay);
    TEST_ASSERT_EQUAL_UINT(1, dieStateTablePtr->dieState[0][3].prevWay);
}

void test_status_check_pass_visits_every_ready_way(void)
{
    KNOWN_BUG("SchedulingNandReqPerCh reads nextWay after moving a way to another list, so each pass handles only the first ready way");
    move_idle_way_to_status_check(0, 0);
    move_idle_way_to_status_check(0, 1);
    SchedulingNandReqPerCh(0);
    TEST_ASSERT_EQUAL_UINT(WAY_NONE, wayPriorityTablePtr->wayPriority[0].statusCheckHead);
    TEST_ASSERT_EQUAL_UINT(0, wayPriorityTablePtr->wayPriority[0].statusReportHead);
    TEST_ASSERT_EQUAL_UINT(1, wayPriorityTablePtr->wayPriority[0].statusReportTail);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_scheduler_init_puts_all_ways_on_idle_list);
    RUN_TEST(test_way_list_selective_removal_keeps_links);
    RUN_TEST(test_status_check_pass_visits_every_ready_way);
    return UNITY_END();
}
