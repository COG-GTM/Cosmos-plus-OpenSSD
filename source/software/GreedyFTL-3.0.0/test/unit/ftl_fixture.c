#include "ftl_fixture.h"

#include "fake_host_dma.h"
#include "host_reg_map.h"
#include "ftl_config.h"
#include "request_schedule.h"

#include <stdio.h>
#include <stdlib.h>
#include <sys/time.h>

static unsigned int fixture_init_count;

void ftl_fixture_init(void)
{
    struct timeval start;
    struct timeval end;
    host_reg_reset();
    fake_host_dma_install();
    fake_nand_stats_reset();
    gettimeofday(&start, NULL);
    InitFTL();
    gettimeofday(&end, NULL);
    if (getenv("HOST_TEST_TIMING") != NULL) {
        double elapsed = (end.tv_sec - start.tv_sec) * 1000.0 +
                         (end.tv_usec - start.tv_usec) / 1000.0;
        fprintf(stderr, "InitFTL fixture call %u: %.3f ms\n", fixture_init_count + 1, elapsed);
    }
    ++fixture_init_count;
    SyncAllLowLevelReqDone();
}

void ftl_fixture_init_fresh_nand(void)
{
    fake_nand_reset();
    ftl_fixture_init();
}
