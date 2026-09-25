#include <assert.h>
#include <string.h>

#include "xparameters.h"
#include "fake_regs.h"
#include "nvme/host_lld.h"

#define REG_TABLE_SIZE			16384	/* power of two; covers the 2048-dword command SRAM plus control registers */
#define WRITE_JOURNAL_SIZE		65536
#define DMA_JOURNAL_SIZE		8192

typedef struct
{
	unsigned int addr;
	unsigned int value;
	unsigned char used;
} REG_SLOT;

static REG_SLOT regTable[REG_TABLE_SIZE];
static FAKE_REG_WRITE writeJournal[WRITE_JOURNAL_SIZE];
static unsigned int writeCount;
static FAKE_DMA_DESCRIPTOR dmaJournal[DMA_JOURNAL_SIZE];
static unsigned int dmaCount;
static unsigned int dmaStaging[4];
static unsigned int completionCount;

static REG_SLOT *lookup(unsigned int addr, int create)
{
	unsigned int index = (addr >> 2) * 2654435761u;
	unsigned int probe;

	for (probe = 0; probe < REG_TABLE_SIZE; probe++)
	{
		REG_SLOT *slot = &regTable[(index + probe) & (REG_TABLE_SIZE - 1)];
		if (slot->used && slot->addr == addr)
			return slot;
		if (!slot->used)
		{
			if (!create)
				return NULL;
			slot->used = 1;
			slot->addr = addr;
			slot->value = 0;
			return slot;
		}
	}
	assert(!"fake register table is full");
	return NULL;
}

void fake_regs_reset(void)
{
	memset(regTable, 0, sizeof(regTable));
	memset(dmaStaging, 0, sizeof(dmaStaging));
	writeCount = 0;
	dmaCount = 0;
	completionCount = 0;
}

unsigned int fake_reg_read32(unsigned int addr)
{
	REG_SLOT *slot = lookup(addr, 0);
	return slot ? slot->value : 0;
}

void fake_reg_poke(unsigned int addr, unsigned int value)
{
	lookup(addr, 1)->value = value;
}

static void completeDma(void)
{
	HOST_DMA_CMD_FIFO_REG reg;
	HOST_DMA_FIFO_CNT_REG cnt;
	FAKE_DMA_DESCRIPTOR *desc;

	memcpy(reg.dword, dmaStaging, sizeof(reg.dword));

	if (dmaCount < DMA_JOURNAL_SIZE)
	{
		desc = &dmaJournal[dmaCount];
		desc->devAddr = reg.devAddr;
		desc->pcieAddrH = reg.pcieAddrH;
		desc->pcieAddrL = reg.pcieAddrL;
		desc->dmaLen = reg.dmaLen;
		desc->autoCompletion = reg.autoCompletion;
		desc->cmd4KBOffset = reg.cmd4KBOffset;
		desc->cmdSlotTag = reg.cmdSlotTag;
		desc->dmaDirection = reg.dmaDirection;
		desc->dmaType = reg.dmaType;
	}
	dmaCount++;

	cnt.dword = fake_reg_read32(HOST_DMA_FIFO_CNT_REG_ADDR);
	if (reg.dmaType == HOST_DMA_DIRECT_TYPE)
	{
		if (reg.dmaDirection == HOST_DMA_TX_DIRECTION)
			cnt.directDmaTx++;
		else
			cnt.directDmaRx++;
	}
	else
	{
		if (reg.dmaDirection == HOST_DMA_TX_DIRECTION)
			cnt.autoDmaTx++;
		else
			cnt.autoDmaRx++;
	}
	fake_reg_poke(HOST_DMA_FIFO_CNT_REG_ADDR, cnt.dword);
}

void fake_reg_write32(unsigned int addr, unsigned int value)
{
	lookup(addr, 1)->value = value;

	if (writeCount < WRITE_JOURNAL_SIZE)
	{
		writeJournal[writeCount].addr = addr;
		writeJournal[writeCount].value = value;
	}
	writeCount++;

	if (addr >= HOST_DMA_CMD_FIFO_REG_ADDR && addr < HOST_DMA_CMD_FIFO_REG_ADDR + 16)
	{
		dmaStaging[(addr - HOST_DMA_CMD_FIFO_REG_ADDR) / 4] = value;
		if (addr == HOST_DMA_CMD_FIFO_REG_ADDR + 12)
			completeDma();
	}
	else if (addr == NVME_CPL_FIFO_REG_ADDR + 8)
		completionCount++;
}

unsigned int fake_reg_write_count(void)
{
	return writeCount;
}

unsigned int fake_reg_write_count_to(unsigned int addr)
{
	unsigned int i, n = 0;
	unsigned int limit = writeCount < WRITE_JOURNAL_SIZE ? writeCount : WRITE_JOURNAL_SIZE;

	for (i = 0; i < limit; i++)
		if (writeJournal[i].addr == addr)
			n++;
	return n;
}

const FAKE_REG_WRITE *fake_reg_write_at(unsigned int index)
{
	if (index >= writeCount || index >= WRITE_JOURNAL_SIZE)
		return NULL;
	return &writeJournal[index];
}

unsigned int fake_reg_last_write_to(unsigned int addr, unsigned int *value)
{
	unsigned int limit = writeCount < WRITE_JOURNAL_SIZE ? writeCount : WRITE_JOURNAL_SIZE;

	while (limit > 0)
	{
		limit--;
		if (writeJournal[limit].addr == addr)
		{
			if (value)
				*value = writeJournal[limit].value;
			return 1;
		}
	}
	return 0;
}

unsigned int fake_dma_descriptor_count(void)
{
	return dmaCount;
}

const FAKE_DMA_DESCRIPTOR *fake_dma_descriptor_at(unsigned int index)
{
	if (index >= dmaCount || index >= DMA_JOURNAL_SIZE)
		return NULL;
	return &dmaJournal[index];
}

unsigned int fake_nvme_completion_count(void)
{
	return completionCount;
}

void fake_nvme_push_command(unsigned int qID, unsigned int cmdSlotTag, unsigned int cmdSeqNum, const unsigned int cmdDword[16])
{
	NVME_CMD_FIFO_REG fifo;
	unsigned int idx;

	for (idx = 0; idx < 16; idx++)
		fake_reg_poke(NVME_CMD_SRAM_ADDR + cmdSlotTag * 64 + idx * 4, cmdDword[idx]);

	fifo.dword = 0;
	fifo.qID = qID;
	fifo.cmdSlotTag = cmdSlotTag;
	fifo.cmdSeqNum = cmdSeqNum;
	fifo.cmdValid = 1;
	fake_reg_poke(NVME_CMD_FIFO_REG_ADDR, fifo.dword);
}
