#include "ftl_test_env.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* InitAddressMap() always looks for the bad-block table in physical block 0. */
#define BBT_PHY_BLOCK 0

/*
 * A missing table would make InitBlockDieMap() scan every page of every block
 * (FindBadBlock), which is far too slow against the in-memory model, so the
 * table is pre-written the way SaveBadBlockTable() would leave it.
 */
static void PreloadBadBlockTables(void)
{
	unsigned int ch, way;
	for (ch = 0; ch < USER_CHANNELS; ch++)
		for (way = 0; way < USER_WAYS; way++)
			fake_nand_preload_bbt(ch, way, BBT_PHY_BLOCK);
}

static void ResetFakes(void)
{
	fake_regs_reset();
	fake_nand_reset();
	fake_dma_reset();
	host_memory_init();
}

static void BringUp(void)
{
	PreloadBadBlockTables();
	InitFTL();
	ftl_test_drain();
	fake_regs_reset();
}

void ftl_test_env_init(void)
{
	ResetFakes();
	BringUp();
}

void ftl_test_env_init_with_bad_block(unsigned int phyBlockNo)
{
	unsigned int ch, way;
	ResetFakes();
	for (ch = 0; ch < USER_CHANNELS; ch++)
		for (way = 0; way < USER_WAYS; way++)
			fake_nand_mark_bad(ch, way, phyBlockNo);
	BringUp();
}

unsigned int ftl_test_pending_nand_reqs(void)
{
	unsigned int ch, way, n = 0;
	for (ch = 0; ch < USER_CHANNELS; ch++)
		for (way = 0; way < USER_WAYS; way++)
			n += nandReqQ[ch][way].reqCnt + blockedByRowAddrDepReqQ[ch][way].reqCnt;
	return n + blockedByBufDepReqQ.reqCnt;
}

void ftl_test_drain(void)
{
	unsigned int iterations = 0;
	while (ftl_test_pending_nand_reqs() > 0 || nvmeDmaReqQ.reqCnt > 0)
	{
		SchedulingNandReq();
		CheckDoneNvmeDmaReq();
		if (++iterations > 10000000u)
		{
			fprintf(stderr, "ftl_test_drain: scheduler did not converge (nand=%u nvme=%u)\n",
					ftl_test_pending_nand_reqs(), nvmeDmaReqQ.reqCnt);
			abort();
		}
	}
}

unsigned int ftl_test_issue_write(unsigned int lsa, unsigned char fillByte)
{
	unsigned int vsa = AddrTransWrite(lsa);
	unsigned int die = Vsa2VdieTranslation(vsa);
	unsigned int tag = GetFromFreeReqQ();
	unsigned int entry = AllocateTempDataBuf(die);

	reqPoolPtr->reqPool[tag].reqType = REQ_TYPE_NAND;
	reqPoolPtr->reqPool[tag].reqCode = REQ_CODE_WRITE;
	reqPoolPtr->reqPool[tag].logicalSliceAddr = lsa;
	reqPoolPtr->reqPool[tag].reqOpt.dataBufFormat = REQ_OPT_DATA_BUF_TEMP_ENTRY;
	reqPoolPtr->reqPool[tag].reqOpt.nandAddr = REQ_OPT_NAND_ADDR_VSA;
	reqPoolPtr->reqPool[tag].reqOpt.nandEcc = REQ_OPT_NAND_ECC_ON;
	reqPoolPtr->reqPool[tag].reqOpt.nandEccWarning = REQ_OPT_NAND_ECC_WARNING_OFF;
	reqPoolPtr->reqPool[tag].reqOpt.rowAddrDependencyCheck = REQ_OPT_ROW_ADDR_DEPENDENCY_CHECK;
	reqPoolPtr->reqPool[tag].reqOpt.blockSpace = REQ_OPT_BLOCK_SPACE_MAIN;
	reqPoolPtr->reqPool[tag].dataBufInfo.entry = entry;
	UpdateTempDataBufEntryInfoBlockingReq(entry, tag);
	reqPoolPtr->reqPool[tag].nandInfo.virtualSliceAddr = vsa;

	memset(host_memory_ptr(GenerateDataBufAddr(tag)), fillByte, BYTES_PER_DATA_REGION_OF_SLICE);
	SelectLowLevelReqQ(tag);
	return tag;
}

unsigned int ftl_test_write_slice(unsigned int lsa, unsigned char fillByte)
{
	unsigned int tag = ftl_test_issue_write(lsa, fillByte);
	unsigned int vsa = reqPoolPtr->reqPool[tag].nandInfo.virtualSliceAddr;
	ftl_test_drain();
	return vsa;
}

unsigned char *ftl_test_nand_page(unsigned int vsa)
{
	unsigned int die = Vsa2VdieTranslation(vsa);
	unsigned int vblock = Vsa2VblockTranslation(vsa);
	unsigned int pblock = phyBlockMapPtr->phyBlock[die][Vblock2PblockOfTbsTranslation(vblock)].remappedPhyBlock;
	unsigned int row = fake_nand_row_addr(pblock, Vpage2PlsbPageTranslation(Vsa2VpageTranslation(vsa)));
	return fake_nand_row(Vdie2PchTranslation(die), Vdie2PwayTranslation(die), row);
}

unsigned int ftl_test_count_free_reqs(void)
{
	unsigned int tag = freeReqQ.headReq, n = 0;
	while (tag != REQ_SLOT_TAG_NONE)
	{
		n++;
		if (n > AVAILABLE_OUNTSTANDING_REQ_COUNT)
			return n;
		tag = reqPoolPtr->reqPool[tag].nextReq;
	}
	return n;
}

int ftl_test_count_lru(void)
{
	unsigned int entry = dataBufLruList.headEntry;
	int n = 0;
	while (entry != DATA_BUF_NONE)
	{
		n++;
		if (n > AVAILABLE_DATA_BUFFER_ENTRY_COUNT)
			return -1;
		entry = dataBufMapPtr->dataBuf[entry].nextEntry;
	}
	return n;
}

unsigned int ftl_test_vsa(unsigned int dieNo, unsigned int blockNo, unsigned int pageNo)
{
	return Vorg2VsaTranslation(dieNo, blockNo, pageNo);
}
