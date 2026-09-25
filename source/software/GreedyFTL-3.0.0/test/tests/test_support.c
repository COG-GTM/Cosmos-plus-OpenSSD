#include "test_support.h"

#include <string.h>

void TestRewindSliceAllocationDie(void)
{
	unsigned int die;
	do
	{
		die = FindDieForFreeSliceAllocation();
	} while (die != USER_DIES - 1);
	sliceAllocationTargetDie = FindDieForFreeSliceAllocation();
	TEST_ASSERT_EQUAL_UINT(0, sliceAllocationTargetDie);
}

void TestInitAddressMapWithoutNandScan(void)
{
	unsigned int dieNo, blockNo;

	logicalSliceMapPtr = (P_LOGICAL_SLICE_MAP)LOGICAL_SLICE_MAP_ADDR;
	virtualSliceMapPtr = (P_VIRTUAL_SLICE_MAP)VIRTUAL_SLICE_MAP_ADDR;
	virtualBlockMapPtr = (P_VIRTUAL_BLOCK_MAP)VIRTUAL_BLOCK_MAP_ADDR;
	virtualDieMapPtr = (P_VIRTUAL_DIE_MAP)VIRTUAL_DIE_MAP_ADDR;
	phyBlockMapPtr = (P_PHY_BLOCK_MAP)PHY_BLOCK_MAP_ADDR;
	bbtInfoMapPtr = (P_BAD_BLOCK_TABLE_INFO_MAP)BAD_BLOCK_TABLE_INFO_MAP_ADDR;

	for (dieNo = 0; dieNo < USER_DIES; dieNo++)
	{
		for (blockNo = 0; blockNo < TOTAL_BLOCKS_PER_DIE; blockNo++)
		{
			phyBlockMapPtr->phyBlock[dieNo][blockNo].remappedPhyBlock = blockNo;
			phyBlockMapPtr->phyBlock[dieNo][blockNo].bad = BLOCK_STATE_NORMAL;
		}
		bbtInfoMapPtr->bbtInfo[dieNo].phyBlock = 0;
		bbtInfoMapPtr->bbtInfo[dieNo].grownBadUpdate = BBT_INFO_GROWN_BAD_UPDATE_NONE;
	}

	mbPerbadBlockSpace = 0;
	TestRewindSliceAllocationDie();

	InitSliceMap();
	InitDieMap();
	InitBlockMap();
	InitCurrentBlockOfDieMap();
}

static void ResetFakesAndTables(int keepNand)
{
	HostMemoryInit();
	HostMemoryClear();
	FakeRegsReset();
	FakeHostDmaReset();
	if (!keepNand)
		FakeNandReset();
	XilStubSetInbyte(0);
	memset(&g_hostDmaStatus, 0, sizeof(g_hostDmaStatus));
	memset(&g_hostDmaAssistStatus, 0, sizeof(g_hostDmaAssistStatus));

	InitChCtlReg();
	InitReqPool();
	InitDependencyTable();
	InitReqScheduler();
}

void TestFtlReset(void)
{
	ResetFakesAndTables(0);
	TestInitAddressMapWithoutNandScan();
	InitDataBuf();
	InitGcVictimMap();
}

void TestFtlResetWithFullInit(void)
{
	ResetFakesAndTables(0);
	TestRewindSliceAllocationDie();
	InitFTL();
}

void TestFtlResetWithFullInitKeepingNand(void)
{
	ResetFakesAndTables(1);
	TestRewindSliceAllocationDie();
	InitFTL();
}

unsigned int TestDieOf(unsigned int vsa) { return Vsa2VdieTranslation(vsa); }
unsigned int TestBlockOf(unsigned int vsa) { return Vsa2VblockTranslation(vsa); }
unsigned int TestPageOf(unsigned int vsa) { return Vsa2VpageTranslation(vsa); }
unsigned int TestChannelOfDie(unsigned int dieNo) { return Vdie2PchTranslation(dieNo); }
unsigned int TestWayOfDie(unsigned int dieNo) { return Vdie2PwayTranslation(dieNo); }

unsigned int TestCountNandReqs(unsigned int chNo, unsigned int wayNo, unsigned int reqCode)
{
	unsigned int tag = nandReqQ[chNo][wayNo].headReq;
	unsigned int n = 0;
	while (tag != REQ_SLOT_TAG_NONE)
	{
		if (reqPoolPtr->reqPool[tag].reqCode == reqCode)
			n++;
		tag = reqPoolPtr->reqPool[tag].nextReq;
	}
	return n;
}

void TestWriteLogicalSlices(unsigned int firstLsa, unsigned int count)
{
	unsigned int i;
	for (i = 0; i < count; i++)
		AddrTransWrite(firstLsa + i);
}
