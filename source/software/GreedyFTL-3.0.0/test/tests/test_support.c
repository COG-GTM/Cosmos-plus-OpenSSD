#include <stdlib.h>
#include <string.h>

#include "test_support.h"

static unsigned char *arenaSnapshot;
static unsigned char sliceAllocationTargetDieSnapshot;
static unsigned int mbPerbadBlockSpaceSnapshot;
static unsigned int storageCapacitySnapshot;

static void resetVolatileState(void)
{
	fake_regs_reset();
	memset(&g_hostDmaStatus, 0, sizeof(g_hostDmaStatus));
	memset(&g_hostDmaAssistStatus, 0, sizeof(g_hostDmaAssistStatus));
	memset((void *)&g_nvmeTask, 0, sizeof(g_nvmeTask));
}

static void alignDieRoundRobin(void)
{
	/* FindDieForFreeSliceAllocation() keeps its cursor in function-local statics; spin it so
	 * that die 0 is the current target and die 1 comes next, whatever earlier tests did. */
	while (FindDieForFreeSliceAllocation() != 0)
		;
	sliceAllocationTargetDie = 0;
}

static void bootFtl(void)
{
	host_mem_clear();
	resetVolatileState();
	xil_stub_set_next_inbyte(0);	/* do not rebuild the bad block table by erasing everything */
	InitFTL();
}

void test_ftl_init_fresh(void)
{
	bootFtl();
	alignDieRoundRobin();
}

void test_ftl_init(void)
{
	size_t size = host_mem_size();

	if (arenaSnapshot == NULL)
	{
		fake_nand_reset();
		bootFtl();
		arenaSnapshot = malloc(size);
		TEST_ASSERT_NOT_NULL_MESSAGE(arenaSnapshot, "cannot allocate arena snapshot");
		memcpy(arenaSnapshot, host_mem_ptr(hostDramBaseAddr), size);
		sliceAllocationTargetDieSnapshot = sliceAllocationTargetDie;
		mbPerbadBlockSpaceSnapshot = mbPerbadBlockSpace;
		storageCapacitySnapshot = storageCapacity_L;
	}
	else
	{
		memcpy(host_mem_ptr(hostDramBaseAddr), arenaSnapshot, size);
		sliceAllocationTargetDie = sliceAllocationTargetDieSnapshot;
		mbPerbadBlockSpace = mbPerbadBlockSpaceSnapshot;
		storageCapacity_L = storageCapacitySnapshot;
	}

	/* Cheap re-initialisation of the tables that live in C globals as well as DRAM. */
	resetVolatileState();
	InitChCtlReg();
	InitReqPool();
	InitDependencyTable();
	InitReqScheduler();
	InitDataBuf();
	InitGcVictimMap();
	fake_nand_reset();
	alignDieRoundRobin();
}

PSA test_vsa_to_psa(unsigned int vsa)
{
	PSA psa;
	unsigned int tempBlockNo, lun;

	psa.dieNo = Vsa2VdieTranslation(vsa);
	psa.chNo = Vdie2PchTranslation(psa.dieNo);
	psa.wayNo = Vdie2PwayTranslation(psa.dieNo);
	psa.virtualBlockNo = Vsa2VblockTranslation(vsa);
	psa.virtualPageNo = Vsa2VpageTranslation(vsa);
	psa.phyBlockNo = phyBlockMapPtr->phyBlock[psa.dieNo][Vblock2PblockOfTbsTranslation(psa.virtualBlockNo)].remappedPhyBlock;

	lun = psa.phyBlockNo / TOTAL_BLOCKS_PER_LUN;
	tempBlockNo = psa.phyBlockNo % TOTAL_BLOCKS_PER_LUN;
	psa.rowAddr = (lun ? LUN_1_BASE_ADDR : LUN_0_BASE_ADDR) + tempBlockNo * PAGES_PER_MLC_BLOCK
		+ Vpage2PlsbPageTranslation(psa.virtualPageNo);
	return psa;
}

unsigned int test_count_free_blocks(unsigned int dieNo)
{
	unsigned int blockNo = virtualDieMapPtr->die[dieNo].headFreeBlock;
	unsigned int count = 0;

	while (blockNo != BLOCK_NONE && count <= USER_BLOCKS_PER_DIE)
	{
		count++;
		blockNo = virtualBlockMapPtr->block[dieNo][blockNo].nextBlock;
	}
	return count;
}

unsigned int test_count_gc_victims(unsigned int dieNo, unsigned int invalidSliceCnt)
{
	unsigned int blockNo = gcVictimMapPtr->gcVictimList[dieNo][invalidSliceCnt].headBlock;
	unsigned int count = 0;

	while (blockNo != BLOCK_NONE && count <= USER_BLOCKS_PER_DIE)
	{
		count++;
		blockNo = virtualBlockMapPtr->block[dieNo][blockNo].nextBlock;
	}
	return count;
}

unsigned int test_count_valid_slices(unsigned int dieNo, unsigned int blockNo)
{
	unsigned int pageNo, vsa, lsa, count = 0;

	for (pageNo = 0; pageNo < USER_PAGES_PER_BLOCK; pageNo++)
	{
		vsa = Vorg2VsaTranslation(dieNo, blockNo, pageNo);
		lsa = virtualSliceMapPtr->virtualSlice[vsa].logicalSliceAddr;
		if (lsa != LSA_NONE && logicalSliceMapPtr->logicalSlice[lsa].virtualSliceAddr == vsa)
			count++;
	}
	return count;
}

unsigned int test_walk_free_req_queue(unsigned int limit)
{
	unsigned int tag = freeReqQ.headReq;
	unsigned int prev = REQ_SLOT_TAG_NONE;
	unsigned int count = 0;

	while (tag != REQ_SLOT_TAG_NONE && count < limit)
	{
		if (reqPoolPtr->reqPool[tag].prevReq != prev)
			return count;
		count++;
		prev = tag;
		tag = reqPoolPtr->reqPool[tag].nextReq;
	}
	return count;
}

void test_drain_nand(void)
{
	SyncAllLowLevelReqDone();
}

unsigned int test_write_slice(unsigned int lsa, unsigned char fill)
{
	unsigned int vsa = AddrTransWrite(lsa);

	test_program_vsa(lsa, vsa, fill);
	return vsa;
}

void test_program_vsa(unsigned int lsa, unsigned int vsa, unsigned char fill)
{
	unsigned int reqSlotTag = GetFromFreeReqQ();
	unsigned int bufEntry = AllocateDataBuf();
	unsigned char *data = host_mem_ptr(DATA_BUFFER_BASE_ADDR + bufEntry * BYTES_PER_DATA_REGION_OF_SLICE);
	unsigned char *spare = host_mem_ptr(SPARE_DATA_BUFFER_BASE_ADDR + bufEntry * BYTES_PER_SPARE_REGION_OF_SLICE);

	memset(data, fill, BYTES_PER_DATA_REGION_OF_SLICE);
	memset(spare, fill, BYTES_PER_SPARE_REGION_OF_SLICE);
	dataBufMapPtr->dataBuf[bufEntry].logicalSliceAddr = lsa;
	dataBufMapPtr->dataBuf[bufEntry].dirty = DATA_BUF_CLEAN;
	PutToDataBufHashList(bufEntry);

	reqPoolPtr->reqPool[reqSlotTag].reqType = REQ_TYPE_NAND;
	reqPoolPtr->reqPool[reqSlotTag].reqCode = REQ_CODE_WRITE;
	reqPoolPtr->reqPool[reqSlotTag].nvmeCmdSlotTag = 0;
	reqPoolPtr->reqPool[reqSlotTag].logicalSliceAddr = lsa;
	reqPoolPtr->reqPool[reqSlotTag].reqOpt.dataBufFormat = REQ_OPT_DATA_BUF_ENTRY;
	reqPoolPtr->reqPool[reqSlotTag].reqOpt.nandAddr = REQ_OPT_NAND_ADDR_VSA;
	reqPoolPtr->reqPool[reqSlotTag].reqOpt.nandEcc = REQ_OPT_NAND_ECC_ON;
	reqPoolPtr->reqPool[reqSlotTag].reqOpt.nandEccWarning = REQ_OPT_NAND_ECC_WARNING_ON;
	reqPoolPtr->reqPool[reqSlotTag].reqOpt.rowAddrDependencyCheck = REQ_OPT_ROW_ADDR_DEPENDENCY_CHECK;
	reqPoolPtr->reqPool[reqSlotTag].reqOpt.blockSpace = REQ_OPT_BLOCK_SPACE_MAIN;
	reqPoolPtr->reqPool[reqSlotTag].dataBufInfo.entry = bufEntry;
	UpdateDataBufEntryInfoBlockingReq(bufEntry, reqSlotTag);
	reqPoolPtr->reqPool[reqSlotTag].nandInfo.virtualSliceAddr = vsa;

	SelectLowLevelReqQ(reqSlotTag);
	test_drain_nand();
}

unsigned char *test_read_slice(unsigned int vsa)
{
	unsigned int reqSlotTag = GetFromFreeReqQ();
	unsigned int bufEntry = AllocateDataBuf();

	dataBufMapPtr->dataBuf[bufEntry].logicalSliceAddr = LSA_NONE;
	dataBufMapPtr->dataBuf[bufEntry].dirty = DATA_BUF_CLEAN;

	reqPoolPtr->reqPool[reqSlotTag].reqType = REQ_TYPE_NAND;
	reqPoolPtr->reqPool[reqSlotTag].reqCode = REQ_CODE_READ;
	reqPoolPtr->reqPool[reqSlotTag].nvmeCmdSlotTag = 0;
	reqPoolPtr->reqPool[reqSlotTag].logicalSliceAddr = virtualSliceMapPtr->virtualSlice[vsa].logicalSliceAddr;
	reqPoolPtr->reqPool[reqSlotTag].reqOpt.dataBufFormat = REQ_OPT_DATA_BUF_ENTRY;
	reqPoolPtr->reqPool[reqSlotTag].reqOpt.nandAddr = REQ_OPT_NAND_ADDR_VSA;
	reqPoolPtr->reqPool[reqSlotTag].reqOpt.nandEcc = REQ_OPT_NAND_ECC_ON;
	reqPoolPtr->reqPool[reqSlotTag].reqOpt.nandEccWarning = REQ_OPT_NAND_ECC_WARNING_ON;
	reqPoolPtr->reqPool[reqSlotTag].reqOpt.rowAddrDependencyCheck = REQ_OPT_ROW_ADDR_DEPENDENCY_CHECK;
	reqPoolPtr->reqPool[reqSlotTag].reqOpt.blockSpace = REQ_OPT_BLOCK_SPACE_MAIN;
	reqPoolPtr->reqPool[reqSlotTag].dataBufInfo.entry = bufEntry;
	UpdateDataBufEntryInfoBlockingReq(bufEntry, reqSlotTag);
	reqPoolPtr->reqPool[reqSlotTag].nandInfo.virtualSliceAddr = vsa;

	SelectLowLevelReqQ(reqSlotTag);
	test_drain_nand();
	return host_mem_ptr(DATA_BUFFER_BASE_ADDR + bufEntry * BYTES_PER_DATA_REGION_OF_SLICE);
}
