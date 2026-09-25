/*
 * Shared fixture for the GreedyFTL host tests.
 *
 * Bringing the FTL up (InitFTL) scans every block of every die for bad-block marks,
 * which takes a few seconds against the fake NAND. test_ftl_init() therefore runs the
 * full boot once per test binary, snapshots the DRAM arena, and restores the snapshot
 * (plus the cheap in-RAM tables and C globals) for every following test.
 * test_ftl_init_fresh() forces a real boot, for tests that change the NAND model
 * (factory bad blocks) before init.
 */
#ifndef TEST_SUPPORT_H
#define TEST_SUPPORT_H

#include <string.h>

#include "unity.h"
#include "memory_map.h"
#include "nvme/nvme.h"
#include "nvme/host_lld.h"
#include "nvme/nvme_admin_cmd.h"
#include "nvme/nvme_io_cmd.h"
#include "nvme/nvme_identify.h"
#include "fake_regs.h"
#include "fake_nand.h"
#include "host_mem.h"
#include "xil_stubs.h"

/* Defined by the firmware but not exported by any header. */
extern volatile NVME_CONTEXT g_nvmeTask;		/* nvme_main.c */
extern P_DATA_BUF_HASH_TABLE dataBufHashTablePtr;	/* data_buffer.c (header declares `dataBufHashTable`) */
void EvictDataBufEntry(unsigned int originReqSlotTag);	/* request_transform.c */
void DataReadFromNand(unsigned int originReqSlotTag);
unsigned int CheckBufDep(unsigned int reqSlotTag);
unsigned int CheckRowAddrDep(unsigned int reqSlotTag, unsigned int checkRowAddrDepOpt);
/* request_schedule.h declares this as dieStatusTablePtr; the definition is dieStateTablePtr */
extern P_DIE_STATE_TABLE dieStateTablePtr;

void test_ftl_init(void);
void test_ftl_init_fresh(void);

/* Physical NAND coordinates of a virtual slice address. */
typedef struct
{
	unsigned int dieNo;
	unsigned int chNo;
	unsigned int wayNo;
	unsigned int virtualBlockNo;
	unsigned int virtualPageNo;
	unsigned int phyBlockNo;
	unsigned int rowAddr;
} PSA;

PSA test_vsa_to_psa(unsigned int vsa);

/* Number of blocks currently linked into a die's free-block list. */
unsigned int test_count_free_blocks(unsigned int dieNo);
/* Number of blocks linked into the GC victim bucket for `invalidSliceCnt`. */
unsigned int test_count_gc_victims(unsigned int dieNo, unsigned int invalidSliceCnt);
/* Number of slices in the block whose mapping is still current. */
unsigned int test_count_valid_slices(unsigned int dieNo, unsigned int blockNo);
/* Walk freeReqQ from head to tail, verifying the links; returns the number of nodes (stops at `limit`). */
unsigned int test_walk_free_req_queue(unsigned int limit);

/* Write logical slice `lsa` through the FTL (AddrTransWrite + NAND program) and wait for completion. */
unsigned int test_write_slice(unsigned int lsa, unsigned char fill);
/* Program `vsa` with `fill` on behalf of `lsa` (maps are not touched) and wait for completion. */
void test_program_vsa(unsigned int lsa, unsigned int vsa, unsigned char fill);
/* Issue a NAND read for `vsa` into a free data buffer entry; returns the data buffer address. */
unsigned char *test_read_slice(unsigned int vsa);

/* Run the NAND scheduler until every outstanding request has completed. */
void test_drain_nand(void);

#endif
