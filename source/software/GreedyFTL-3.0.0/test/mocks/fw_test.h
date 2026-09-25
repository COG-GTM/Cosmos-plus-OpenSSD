/*
 * Common per-test fixture for GreedyFTL host tests.
 *
 * fw_test_reset() puts every mock, stub and the firmware DRAM image back into
 * a known state. Call it from setUp(). fw_test_init_ftl() then runs the real
 * InitFTL() against the ideal simulated NAND (mock_nsc defaults), which is the
 * fastest way to get fully initialised FTL metadata. It clears the mock_nsc
 * call log afterwards, so tests only see NAND calls made by the code under
 * test; call InitFTL() directly to inspect initialisation traffic.
 */
#ifndef FW_TEST_H
#define FW_TEST_H

#include "fw_assert.h"
#include "fw_memory.h"
#include "mock_host_lld.h"
#include "mock_io.h"
#include "mock_nsc.h"

void fw_test_reset(void);
void fw_test_init_ftl(void);

#endif /* FW_TEST_H */
