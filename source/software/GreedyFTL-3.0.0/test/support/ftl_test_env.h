#ifndef FTL_TEST_ENV_H_
#define FTL_TEST_ENV_H_

/*
 * Shared fixture for GreedyFTL host tests.
 *
 * ftl_test_env_init() brings the whole FTL up exactly the way main.c does on
 * the board (InitFTL), but on top of the fake NAND / DMA / register layers,
 * then drains the reset/set-feature NAND requests so the die schedulers are
 * idle. Each call resets all fakes first, so tests are independent.
 */

#include "address_translation.h"
#include "data_buffer.h"
#include "fake_dma.h"
#include "fake_nand.h"
#include "fake_regs.h"
#include "ftl_config.h"
#include "garbage_collection.h"
#include "host_memory.h"
#include "memory_map.h"
#include "request_allocation.h"
#include "request_format.h"
#include "request_queue.h"
#include "request_schedule.h"
#include "request_transform.h"
#include "xparameters.h"
#include "nvme/host_lld.h"
#include "nvme/nvme.h"

/* data_buffer.h declares `dataBufHashTable`, but data_buffer.c defines
 * `dataBufHashTablePtr`; the real symbol is exposed here for tests. */
extern P_DATA_BUF_HASH_TABLE dataBufHashTablePtr;

void ftl_test_env_init(void);

/*
 * Like ftl_test_env_init() but marks the given physical block bad on every die
 * before the FTL initialises, so bad-block remapping is exercised.
 */
void ftl_test_env_init_with_bad_block(unsigned int phyBlockNo);

/* Same as above for several physical blocks (all marked on every die). */
void ftl_test_env_init_with_bad_blocks(const unsigned int *phyBlockNos, unsigned int count);

/*
 * Split bring-up for tests that need to shape the fake NAND before InitFTL():
 * reset the fakes, optionally preload the bad-block table on every die except
 * (skipCh, skipWay) (pass -1 for both to preload all dies), then run InitFTL.
 */
void ftl_test_env_reset_fakes(void);
void ftl_test_env_preload_bbt_except(int skipCh, int skipWay);
void ftl_test_env_bring_up(void);

/*
 * Run `fn` in a forked child and report whether it died from abort()
 * (i.e. hit an ASSERT/assert). gcov counters of the child are flushed before
 * it exits so the lines leading up to the assertion are still counted.
 */
int ftl_test_expect_abort(void (*fn)(void));

/* Pointer to the on-NAND bad-block table entry for `phyBlockNo` on die (ch, way). */
unsigned char *ftl_test_bbt_entry(unsigned int ch, unsigned int way, unsigned int bbtPhyBlock, unsigned int phyBlockNo);

/* Run the request scheduler until every NAND/NVMe request queue is empty. */
void ftl_test_drain(void);

/* Number of requests currently queued for NAND on any die (nandReqQ + blocked/blocking). */
unsigned int ftl_test_pending_nand_reqs(void);

/* Count entries in the free request queue by walking it. */
unsigned int ftl_test_count_free_reqs(void);

/* Maps `lsa`, programs a page filled with `fillByte` through the real
 * scheduler and waits for it to complete. Returns the VSA that was written. */
unsigned int ftl_test_write_slice(unsigned int lsa, unsigned char fillByte);

/* Same as ftl_test_write_slice but returns the request tag without draining. */
unsigned int ftl_test_issue_write(unsigned int lsa, unsigned char fillByte);

/* Contents of the fake NAND page backing `vsa` (follows the remap table). */
unsigned char *ftl_test_nand_page(unsigned int vsa);

/* Count entries in the data buffer LRU list, or -1 if the list is malformed/cyclic. */
int ftl_test_count_lru(void);

/* Slice/die geometry helpers so tests do not repeat the address arithmetic. */
unsigned int ftl_test_vsa(unsigned int dieNo, unsigned int blockNo, unsigned int pageNo);

#endif
