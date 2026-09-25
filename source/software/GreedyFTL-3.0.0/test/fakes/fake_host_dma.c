#include "fake_host_dma.h"

#include <string.h>

#include "fake_regs.h"
#include "xparameters.h"
#include "nvme/host_lld.h"

#define FAKE_DMA_LOG_CAPACITY 8192u

static FakeDmaDescriptor descriptors[FAKE_DMA_LOG_CAPACITY];
static unsigned int descriptorCount;
static HOST_DMA_CMD_FIFO_REG pendingCmd;
static HOST_DMA_FIFO_CNT_REG head;

static void CompleteDescriptor(const HOST_DMA_CMD_FIFO_REG *cmd)
{
	if (descriptorCount < FAKE_DMA_LOG_CAPACITY)
	{
		FakeDmaDescriptor *d = &descriptors[descriptorCount];
		d->devAddr = cmd->devAddr;
		d->pcieAddrH = cmd->pcieAddrH;
		d->pcieAddrL = cmd->pcieAddrL;
		d->dmaLen = cmd->dmaLen;
		d->autoCompletion = cmd->autoCompletion;
		d->cmd4KBOffset = cmd->cmd4KBOffset;
		d->cmdSlotTag = cmd->cmdSlotTag;
		d->dmaDirection = cmd->dmaDirection;
		d->dmaType = cmd->dmaType;
	}
	descriptorCount++;

	if (cmd->dmaType == HOST_DMA_DIRECT_TYPE)
	{
		if (cmd->dmaDirection == HOST_DMA_TX_DIRECTION)
			head.directDmaTx++;
		else
			head.directDmaRx++;
	}
	else
	{
		if (cmd->dmaDirection == HOST_DMA_TX_DIRECTION)
			head.autoDmaTx++;
		else
			head.autoDmaRx++;
	}
	FakeRegPoke(HOST_DMA_FIFO_CNT_REG_ADDR, head.dword);
}

static void OnRegisterWrite(unsigned int addr, unsigned int value)
{
	if (addr == HOST_DMA_CMD_FIFO_REG_ADDR)
		pendingCmd.dword[0] = value;
	else if (addr == HOST_DMA_CMD_FIFO_REG_ADDR + 4)
		pendingCmd.dword[1] = value;
	else if (addr == HOST_DMA_CMD_FIFO_REG_ADDR + 8)
		pendingCmd.dword[2] = value;
	else if (addr == HOST_DMA_CMD_FIFO_REG_ADDR + 12)
	{
		pendingCmd.dword[3] = value;
		CompleteDescriptor(&pendingCmd);
	}
}

void FakeHostDmaReset(void)
{
	memset(descriptors, 0, sizeof(descriptors));
	memset(&pendingCmd, 0, sizeof(pendingCmd));
	descriptorCount = 0;
	head.dword = 0;
	FakeRegPoke(HOST_DMA_FIFO_CNT_REG_ADDR, 0);
	FakeRegsSetWriteHook(OnRegisterWrite);
}

unsigned int FakeHostDmaCount(void)
{
	return descriptorCount;
}

FakeDmaDescriptor FakeHostDmaAt(unsigned int index)
{
	FakeDmaDescriptor none;
	memset(&none, 0, sizeof(none));
	if (index >= descriptorCount || index >= FAKE_DMA_LOG_CAPACITY)
		return none;
	return descriptors[index];
}

unsigned int FakeHostDmaCountFor(unsigned int dmaType, unsigned int dmaDirection)
{
	unsigned int i, n = 0;
	unsigned int limit = descriptorCount < FAKE_DMA_LOG_CAPACITY ? descriptorCount : FAKE_DMA_LOG_CAPACITY;
	for (i = 0; i < limit; i++)
		if (descriptors[i].dmaType == dmaType && descriptors[i].dmaDirection == dmaDirection)
			n++;
	return n;
}
