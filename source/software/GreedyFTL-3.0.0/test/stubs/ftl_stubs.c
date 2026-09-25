// Test doubles for the FTL modules that garbage_collection.c depends on.
// The map tables are heap allocated; the doubles record the calls they receive.
#include <stdlib.h>
#include <string.h>
#include "memory_map.h"
#include "ftl_stubs.h"

P_LOGICAL_SLICE_MAP logicalSliceMapPtr;
P_VIRTUAL_SLICE_MAP virtualSliceMapPtr;
P_VIRTUAL_BLOCK_MAP virtualBlockMapPtr;
P_REQ_POOL reqPoolPtr;

STUB_STATE stub;

static GC_VICTIM_MAP gcVictimMapStorage;

void stub_reset(void)
{
	if(logicalSliceMapPtr == NULL)
	{
		logicalSliceMapPtr = calloc(1, sizeof(LOGICAL_SLICE_MAP));
		virtualSliceMapPtr = calloc(1, sizeof(VIRTUAL_SLICE_MAP));
		virtualBlockMapPtr = calloc(1, sizeof(VIRTUAL_BLOCK_MAP));
		reqPoolPtr = calloc(1, sizeof(REQ_POOL));
	}
	memset(logicalSliceMapPtr, 0xff, sizeof(LOGICAL_SLICE_MAP));
	memset(virtualSliceMapPtr, 0xff, sizeof(VIRTUAL_SLICE_MAP));
	memset(virtualBlockMapPtr, 0, sizeof(VIRTUAL_BLOCK_MAP));
	memset(reqPoolPtr, 0, sizeof(REQ_POOL));
	memset(&gcVictimMapStorage, 0xff, sizeof(gcVictimMapStorage));
	gcVictimMapPtr = &gcVictimMapStorage;
	memset(&stub, 0, sizeof(stub));
	stub.nextFreeVsa = STUB_FREE_VSA_BASE;
}

unsigned int GetFromFreeReqQ(void)
{
	return stub.nextReqSlotTag++ % AVAILABLE_OUNTSTANDING_REQ_COUNT;
}

unsigned int AllocateTempDataBuf(unsigned int dieNo)
{
	stub.allocateTempDataBufCalls++;
	return dieNo;
}

void UpdateTempDataBufEntryInfoBlockingReq(unsigned int bufEntry, unsigned int reqSlotTag)
{
	(void)bufEntry;
	(void)reqSlotTag;
	stub.updateTempDataBufCalls++;
}

unsigned int FindFreeVirtualSliceForGc(unsigned int copyTargetDieNo, unsigned int victimBlockNo)
{
	stub.findFreeVsaForGcCalls++;
	stub.lastGcCopyDie = copyTargetDieNo;
	stub.lastGcVictimBlock = victimBlockNo;
	return stub.nextFreeVsa++;
}

void SelectLowLevelReqQ(unsigned int reqSlotTag)
{
	if(stub.selectLowLevelReqQCalls < STUB_MAX_RECORDED_REQS)
		stub.issuedReqSlotTags[stub.selectLowLevelReqQCalls] = reqSlotTag;
	stub.selectLowLevelReqQCalls++;
}

void EraseBlock(unsigned int dieNo, unsigned int blockNo)
{
	stub.eraseBlockCalls++;
	stub.lastEraseDie = dieNo;
	stub.lastEraseBlock = blockNo;
}
