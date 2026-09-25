/* Shared fixtures for the GreedyFTL host unit tests. */
#ifndef TEST_SUPPORT_H
#define TEST_SUPPORT_H

#include "unity.h"
#include "memory_map.h"
#include "nvme/nvme.h"
#include "nvme/host_lld.h"
#include "fake_regs.h"
#include "fake_nand.h"
#include "fake_host_dma.h"
#include "host_memory.h"
#include "xil_printf.h"

/* Firmware functions that are external but not declared in any firmware header. */
void InitDieMap(void);
void InitBlockMap(void);
void InitCurrentBlockOfDieMap(void);
void RemapBadBlock(void);
void EvictDataBufEntry(unsigned int originReqSlotTag);
void DataReadFromNand(unsigned int originReqSlotTag);
/* data_buffer.h declares `dataBufHashTable`, but data_buffer.c defines `dataBufHashTablePtr`. */
extern P_DATA_BUF_HASH_TABLE dataBufHashTablePtr;

/* Resets every fake and brings the FTL to a freshly-initialised state WITHOUT
 * scanning the NAND: all blocks are treated as good and erased. Fast enough to
 * run from Unity's setUp(). */
void TestFtlReset(void);

/* Full firmware bring-up (InitFTL) against the fake NAND, including the
 * bad-block scan. Several seconds; use only in the tests that need it. */
void TestFtlResetWithFullInit(void);

/* Like TestFtlResetWithFullInit() but keeps the fake NAND contents (simulates a reboot). */
void TestFtlResetWithFullInitKeepingNand(void);

/* Address-map-only initialisation used by TestFtlReset(). */
void TestInitAddressMapWithoutNandScan(void);

/* Rewinds the round-robin die selector so the next FindFreeVirtualSlice() targets die 0. */
void TestRewindSliceAllocationDie(void);

/* Convenience accessors. */
unsigned int TestDieOf(unsigned int vsa);
unsigned int TestBlockOf(unsigned int vsa);
unsigned int TestPageOf(unsigned int vsa);
unsigned int TestChannelOfDie(unsigned int dieNo);
unsigned int TestWayOfDie(unsigned int dieNo);

/* Number of requests waiting in nandReqQ[ch][way] with the given reqCode. */
unsigned int TestCountNandReqs(unsigned int chNo, unsigned int wayNo, unsigned int reqCode);

/* Writes `count` logical slices starting at `firstLsa` through AddrTransWrite(). */
void TestWriteLogicalSlices(unsigned int firstLsa, unsigned int count);

#endif
