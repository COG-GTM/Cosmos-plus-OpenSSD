/*
 * Fake NVMe host controller low-level driver.
 *
 * Replaces nvme/host_lld.c for host builds. The register traffic of the real
 * driver is preserved (through IO_WRITE32 -> fake_regs) so tests can assert
 * on it, and the DMA FIFO bookkeeping in g_hostDmaStatus is kept identical so
 * request_transform.c's partial-done checks run on the same state machine.
 * Instead of a PCIe endpoint, completions are modelled by advancing the FIFO
 * head, and each descriptor is logged for inspection via fake_dma.h.
 */

#include "fake_dma.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "fake_regs.h"
#include "debug.h"
#include "io_access.h"
#include "nvme.h"
#include "xparameters.h"
#include "host_lld.h"
#include "xil_printf.h"

#define DMA_LOG_MAX 65536
#define CPL_LOG_MAX 4096

HOST_DMA_STATUS g_hostDmaStatus;
HOST_DMA_ASSIST_STATUS g_hostDmaAssistStatus;

static FAKE_DMA_DESC dmaLog[DMA_LOG_MAX];
static unsigned int dmaCount = 0;
static FAKE_NVME_CPL cplLog[CPL_LOG_MAX];
static unsigned int cplCount = 0;
static int autoComplete = 1;

static void PublishFifoHead(void)
{
	fake_reg_set(HOST_DMA_FIFO_CNT_REG_ADDR, g_hostDmaStatus.fifoHead.dword);
}

static void CompleteIfAuto(void)
{
	if (autoComplete)
		fake_dma_complete_all();
	else
		PublishFifoHead();
}

static void LogDma(const HOST_DMA_CMD_FIFO_REG *reg)
{
	if (dmaCount < DMA_LOG_MAX)
	{
		FAKE_DMA_DESC *d = &dmaLog[dmaCount];
		d->dmaType = reg->dmaType;
		d->dmaDirection = reg->dmaDirection;
		d->devAddr = reg->devAddr;
		d->pcieAddrH = reg->pcieAddrH;
		d->pcieAddrL = reg->pcieAddrL;
		d->len = reg->dmaLen;
		d->cmdSlotTag = reg->cmdSlotTag;
		d->cmd4KBOffset = reg->cmd4KBOffset;
		d->autoCompletion = reg->autoCompletion;
	}
	dmaCount++;
}

static void LogCpl(unsigned int cplType, unsigned int cmdSlotTag, unsigned int sqId, unsigned int cid, unsigned int specific, unsigned int statusFieldWord)
{
	if (cplCount < CPL_LOG_MAX)
	{
		FAKE_NVME_CPL *c = &cplLog[cplCount];
		c->cplType = cplType;
		c->cmdSlotTag = cmdSlotTag;
		c->sqId = sqId;
		c->cid = cid;
		c->specific = specific;
		c->statusFieldWord = statusFieldWord;
	}
	cplCount++;
}

/* ---- inspection API ----------------------------------------------------- */

void fake_dma_reset(void)
{
	memset(&g_hostDmaStatus, 0, sizeof(g_hostDmaStatus));
	memset(&g_hostDmaAssistStatus, 0, sizeof(g_hostDmaAssistStatus));
	dmaCount = 0;
	cplCount = 0;
	autoComplete = 1;
	PublishFifoHead();
}

void fake_dma_set_auto_complete(int enabled)
{
	autoComplete = enabled;
}

void fake_dma_complete_all(void)
{
	g_hostDmaStatus.fifoHead.dword = g_hostDmaStatus.fifoTail.dword;
	PublishFifoHead();
}

unsigned int fake_dma_count(void)
{
	return dmaCount;
}

FAKE_DMA_DESC fake_dma_at(unsigned int index)
{
	FAKE_DMA_DESC none;
	memset(&none, 0, sizeof(none));
	if (index >= dmaCount || index >= DMA_LOG_MAX)
		return none;
	return dmaLog[index];
}

unsigned int fake_dma_count_of(unsigned int dmaType, unsigned int dmaDirection)
{
	unsigned int i, n = 0, limit = dmaCount < DMA_LOG_MAX ? dmaCount : DMA_LOG_MAX;
	for (i = 0; i < limit; i++)
		if (dmaLog[i].dmaType == dmaType && dmaLog[i].dmaDirection == dmaDirection)
			n++;
	return n;
}

unsigned int fake_nvme_cpl_count(void)
{
	return cplCount;
}

FAKE_NVME_CPL fake_nvme_cpl_at(unsigned int index)
{
	FAKE_NVME_CPL none;
	memset(&none, 0, sizeof(none));
	if (index >= cplCount || index >= CPL_LOG_MAX)
		return none;
	return cplLog[index];
}

void fake_nvme_push_cmd(unsigned int qID, unsigned int cmdSlotTag, unsigned int cmdSeqNum, const unsigned int cmdDword[16])
{
	NVME_CMD_FIFO_REG reg;
	unsigned int i;

	for (i = 0; i < 16; i++)
		fake_reg_set(NVME_CMD_SRAM_ADDR + cmdSlotTag * 64 + i * 4, cmdDword[i]);

	reg.dword = 0;
	reg.qID = qID;
	reg.cmdSlotTag = cmdSlotTag;
	reg.cmdSeqNum = cmdSeqNum;
	reg.cmdValid = 1;
	fake_reg_set(NVME_CMD_FIFO_REG_ADDR, reg.dword);
}

/* ---- host_lld.h API ----------------------------------------------------- */

void dev_irq_init()
{
	DEV_IRQ_REG devReg;
	devReg.dword = 0;
	devReg.pcieLink = 1;
	devReg.busMaster = 1;
	devReg.pcieIrq = 1;
	devReg.pcieMsi = 1;
	devReg.pcieMsix = 1;
	devReg.nvmeCcEn = 1;
	devReg.nvmeCcShn = 1;
	devReg.mAxiWriteErr = 1;
	devReg.pcieMreqErr = 1;
	devReg.pcieCpldErr = 1;
	devReg.pcieCpldLenErr = 1;
	IO_WRITE32(DEV_IRQ_MASK_REG_ADDR, devReg.dword);
}

void dev_irq_handler()
{
	DEV_IRQ_REG devReg;
	devReg.dword = IO_READ32(DEV_IRQ_STATUS_REG_ADDR);
	IO_WRITE32(DEV_IRQ_CLEAR_REG_ADDR, devReg.dword);
}

unsigned int check_nvme_cc_en()
{
	NVME_STATUS_REG nvmeReg;
	nvmeReg.dword = IO_READ32(NVME_STATUS_REG_ADDR);
	return (unsigned int)nvmeReg.ccEn;
}

void set_nvme_csts_rdy(unsigned int rdy)
{
	NVME_STATUS_REG nvmeReg;
	nvmeReg.dword = IO_READ32(NVME_STATUS_REG_ADDR);
	nvmeReg.cstsRdy = rdy;
	IO_WRITE32(NVME_STATUS_REG_ADDR, nvmeReg.dword);
}

void set_nvme_csts_shst(unsigned int shst)
{
	NVME_STATUS_REG nvmeReg;
	nvmeReg.dword = IO_READ32(NVME_STATUS_REG_ADDR);
	nvmeReg.cstsShst = shst;
	IO_WRITE32(NVME_STATUS_REG_ADDR, nvmeReg.dword);
}

void set_nvme_admin_queue(unsigned int sqValid, unsigned int cqValid, unsigned int cqIrqEn)
{
	NVME_ADMIN_QUEUE_SET_REG reg;
	reg.dword = IO_READ32(NVME_ADMIN_QUEUE_SET_REG_ADDR);
	reg.sqValid = sqValid;
	reg.cqValid = cqValid;
	reg.cqIrqEn = cqIrqEn;
	IO_WRITE32(NVME_ADMIN_QUEUE_SET_REG_ADDR, reg.dword);
}

unsigned int get_nvme_cmd(unsigned short *qID, unsigned short *cmdSlotTag, unsigned int *cmdSeqNum, unsigned int *cmdDword)
{
	NVME_CMD_FIFO_REG reg;
	unsigned int i;

	reg.dword = IO_READ32(NVME_CMD_FIFO_REG_ADDR);
	if (!reg.cmdValid)
		return 0;

	*qID = reg.qID;
	*cmdSlotTag = reg.cmdSlotTag;
	*cmdSeqNum = reg.cmdSeqNum;
	for (i = 0; i < 16; i++)
		cmdDword[i] = IO_READ32(NVME_CMD_SRAM_ADDR + reg.cmdSlotTag * 64 + i * 4);

	/* The FIFO pops on read: clear the valid bit until the test pushes another command. */
	fake_reg_set(NVME_CMD_FIFO_REG_ADDR, 0);
	return 1;
}

void set_auto_nvme_cpl(unsigned int cmdSlotTag, unsigned int specific, unsigned int statusFieldWord)
{
	NVME_CPL_FIFO_REG reg;
	reg.dword[0] = 0;
	reg.dword[1] = specific;
	reg.dword[2] = 0;
	reg.cmdSlotTag = cmdSlotTag;
	reg.cplType = AUTO_CPL_TYPE;
	reg.statusFieldWord = statusFieldWord;
	IO_WRITE32(NVME_CPL_FIFO_REG_ADDR, reg.dword[1]);
	IO_WRITE32((NVME_CPL_FIFO_REG_ADDR + 8), reg.dword[2]);
	LogCpl(AUTO_CPL_TYPE, cmdSlotTag, 0, 0, specific, statusFieldWord);
}

void set_nvme_slot_release(unsigned int cmdSlotTag)
{
	NVME_CPL_FIFO_REG reg;
	reg.dword[2] = 0;
	reg.cmdSlotTag = cmdSlotTag;
	reg.cplType = CMD_SLOT_RELEASE_TYPE;
	IO_WRITE32((NVME_CPL_FIFO_REG_ADDR + 8), reg.dword[2]);
	LogCpl(CMD_SLOT_RELEASE_TYPE, cmdSlotTag, 0, 0, 0, 0);
}

void set_nvme_cpl(unsigned int sqId, unsigned int cid, unsigned int specific, unsigned int statusFieldWord)
{
	NVME_CPL_FIFO_REG reg;
	reg.dword[0] = 0;
	reg.dword[1] = specific;
	reg.dword[2] = 0;
	reg.cid = cid;
	reg.sqId = sqId;
	reg.cplType = ONLY_CPL_TYPE;
	reg.statusFieldWord = statusFieldWord;
	IO_WRITE32(NVME_CPL_FIFO_REG_ADDR, reg.dword[0]);
	IO_WRITE32((NVME_CPL_FIFO_REG_ADDR + 4), reg.dword[1]);
	IO_WRITE32((NVME_CPL_FIFO_REG_ADDR + 8), reg.dword[2]);
	LogCpl(ONLY_CPL_TYPE, 0, sqId, cid, specific, statusFieldWord);
}

void set_io_sq(unsigned int ioSqIdx, unsigned int valid, unsigned int cqVector, unsigned int qSzie, unsigned int pcieBaseAddrL, unsigned int pcieBaseAddrH)
{
	NVME_IO_SQ_SET_REG reg;
	reg.dword[0] = 0;
	reg.dword[1] = 0;
	reg.pcieBaseAddrL = pcieBaseAddrL;
	reg.pcieBaseAddrH = pcieBaseAddrH;
	reg.valid = valid;
	reg.cqVector = cqVector;
	reg.sqSize = qSzie;
	IO_WRITE32((NVME_IO_SQ_SET_REG_ADDR + (ioSqIdx * 8)), reg.dword[0]);
	IO_WRITE32((NVME_IO_SQ_SET_REG_ADDR + (ioSqIdx * 8) + 4), reg.dword[1]);
}

void set_io_cq(unsigned int ioCqIdx, unsigned int valid, unsigned int irqEn, unsigned int irqVector, unsigned int qSzie, unsigned int pcieBaseAddrL, unsigned int pcieBaseAddrH)
{
	NVME_IO_CQ_SET_REG reg;
	reg.dword[0] = 0;
	reg.dword[1] = 0;
	reg.pcieBaseAddrL = pcieBaseAddrL;
	reg.pcieBaseAddrH = pcieBaseAddrH;
	reg.valid = valid;
	reg.irqEn = irqEn;
	reg.irqVector = irqVector;
	reg.cqSize = qSzie;
	IO_WRITE32((NVME_IO_CQ_SET_REG_ADDR + (ioCqIdx * 8)), reg.dword[0]);
	IO_WRITE32((NVME_IO_CQ_SET_REG_ADDR + (ioCqIdx * 8) + 4), reg.dword[1]);
}

static void IssueDma(HOST_DMA_CMD_FIFO_REG *reg)
{
	IO_WRITE32(HOST_DMA_CMD_FIFO_REG_ADDR, reg->dword[0]);
	IO_WRITE32((HOST_DMA_CMD_FIFO_REG_ADDR + 4), reg->dword[1]);
	IO_WRITE32((HOST_DMA_CMD_FIFO_REG_ADDR + 8), reg->dword[2]);
	IO_WRITE32((HOST_DMA_CMD_FIFO_REG_ADDR + 12), reg->dword[3]);
	LogDma(reg);
}

void set_direct_tx_dma(unsigned int devAddr, unsigned int pcieAddrH, unsigned int pcieAddrL, unsigned int len)
{
	HOST_DMA_CMD_FIFO_REG reg;
	ASSERT(len <= 0x1000);
	reg.devAddr = devAddr;
	reg.pcieAddrH = pcieAddrH;
	reg.pcieAddrL = pcieAddrL;
	reg.dword[3] = 0;
	reg.dmaType = HOST_DMA_DIRECT_TYPE;
	reg.dmaDirection = HOST_DMA_TX_DIRECTION;
	reg.dmaLen = len;
	IssueDma(&reg);
	g_hostDmaStatus.fifoTail.directDmaTx++;
	g_hostDmaStatus.directDmaTxCnt++;
	CompleteIfAuto();
}

void set_direct_rx_dma(unsigned int devAddr, unsigned int pcieAddrH, unsigned int pcieAddrL, unsigned int len)
{
	HOST_DMA_CMD_FIFO_REG reg;
	ASSERT(len <= 0x1000);
	reg.devAddr = devAddr;
	reg.pcieAddrH = pcieAddrH;
	reg.pcieAddrL = pcieAddrL;
	reg.dword[3] = 0;
	reg.dmaType = HOST_DMA_DIRECT_TYPE;
	reg.dmaDirection = HOST_DMA_RX_DIRECTION;
	reg.dmaLen = len;
	IssueDma(&reg);
	g_hostDmaStatus.fifoTail.directDmaRx++;
	g_hostDmaStatus.directDmaRxCnt++;
	CompleteIfAuto();
}

void set_auto_tx_dma(unsigned int cmdSlotTag, unsigned int cmd4KBOffset, unsigned int devAddr, unsigned int autoCompletion)
{
	HOST_DMA_CMD_FIFO_REG reg;
	unsigned char tempTail;

	ASSERT(cmd4KBOffset < 256);
	reg.devAddr = devAddr;
	reg.pcieAddrH = 0;
	reg.pcieAddrL = 0;
	reg.dword[3] = 0;
	reg.dmaType = HOST_DMA_AUTO_TYPE;
	reg.dmaDirection = HOST_DMA_TX_DIRECTION;
	reg.cmd4KBOffset = cmd4KBOffset;
	reg.cmdSlotTag = cmdSlotTag;
	reg.autoCompletion = autoCompletion;
	IssueDma(&reg);

	tempTail = g_hostDmaStatus.fifoTail.autoDmaTx++;
	if (tempTail > g_hostDmaStatus.fifoTail.autoDmaTx)
		g_hostDmaAssistStatus.autoDmaTxOverFlowCnt++;
	g_hostDmaStatus.autoDmaTxCnt++;
	CompleteIfAuto();
}

void set_auto_rx_dma(unsigned int cmdSlotTag, unsigned int cmd4KBOffset, unsigned int devAddr, unsigned int autoCompletion)
{
	HOST_DMA_CMD_FIFO_REG reg;
	unsigned char tempTail;

	ASSERT(cmd4KBOffset < 256);
	reg.devAddr = devAddr;
	reg.pcieAddrH = 0;
	reg.pcieAddrL = 0;
	reg.dword[3] = 0;
	reg.dmaType = HOST_DMA_AUTO_TYPE;
	reg.dmaDirection = HOST_DMA_RX_DIRECTION;
	reg.cmd4KBOffset = cmd4KBOffset;
	reg.cmdSlotTag = cmdSlotTag;
	reg.autoCompletion = autoCompletion;
	IssueDma(&reg);

	tempTail = g_hostDmaStatus.fifoTail.autoDmaRx++;
	if (tempTail > g_hostDmaStatus.fifoTail.autoDmaRx)
		g_hostDmaAssistStatus.autoDmaRxOverFlowCnt++;
	g_hostDmaStatus.autoDmaRxCnt++;
	CompleteIfAuto();
}

static void RefreshHead(void)
{
	g_hostDmaStatus.fifoHead.dword = IO_READ32(HOST_DMA_FIFO_CNT_REG_ADDR);
}

void check_direct_tx_dma_done()
{
	RefreshHead();
	if (g_hostDmaStatus.fifoHead.directDmaTx != g_hostDmaStatus.fifoTail.directDmaTx)
	{
		fprintf(stderr, "fake_host_lld: direct TX DMA not complete (auto-complete disabled)\n");
		abort();
	}
}

void check_direct_rx_dma_done()
{
	RefreshHead();
	if (g_hostDmaStatus.fifoHead.directDmaRx != g_hostDmaStatus.fifoTail.directDmaRx)
	{
		fprintf(stderr, "fake_host_lld: direct RX DMA not complete (auto-complete disabled)\n");
		abort();
	}
}

void check_auto_tx_dma_done()
{
	RefreshHead();
	if (g_hostDmaStatus.fifoHead.autoDmaTx != g_hostDmaStatus.fifoTail.autoDmaTx)
	{
		fprintf(stderr, "fake_host_lld: auto TX DMA not complete (auto-complete disabled)\n");
		abort();
	}
}

void check_auto_rx_dma_done()
{
	RefreshHead();
	if (g_hostDmaStatus.fifoHead.autoDmaRx != g_hostDmaStatus.fifoTail.autoDmaRx)
	{
		fprintf(stderr, "fake_host_lld: auto RX DMA not complete (auto-complete disabled)\n");
		abort();
	}
}

/* Partial-done logic copied verbatim from host_lld.c so the ring-wrap arithmetic is exercised. */
unsigned int check_auto_tx_dma_partial_done(unsigned int tailIndex, unsigned int tailAssistIndex)
{
	RefreshHead();

	if (g_hostDmaStatus.fifoHead.autoDmaTx == g_hostDmaStatus.fifoTail.autoDmaTx)
		return 1;

	if (g_hostDmaStatus.fifoHead.autoDmaTx < tailIndex)
	{
		if (g_hostDmaStatus.fifoTail.autoDmaTx < tailIndex)
		{
			if (g_hostDmaStatus.fifoTail.autoDmaTx > g_hostDmaStatus.fifoHead.autoDmaTx)
				return 1;
			else if (g_hostDmaAssistStatus.autoDmaTxOverFlowCnt != (tailAssistIndex + 1))
				return 1;
		}
		else if (g_hostDmaAssistStatus.autoDmaTxOverFlowCnt != tailAssistIndex)
			return 1;
	}
	else if (g_hostDmaStatus.fifoHead.autoDmaTx == tailIndex)
		return 1;
	else
	{
		if (g_hostDmaStatus.fifoTail.autoDmaTx < tailIndex)
			return 1;
		else if (g_hostDmaStatus.fifoTail.autoDmaTx > g_hostDmaStatus.fifoHead.autoDmaTx)
			return 1;
		else if (g_hostDmaAssistStatus.autoDmaTxOverFlowCnt != tailAssistIndex)
			return 1;
	}
	return 0;
}

unsigned int check_auto_rx_dma_partial_done(unsigned int tailIndex, unsigned int tailAssistIndex)
{
	RefreshHead();

	if (g_hostDmaStatus.fifoHead.autoDmaRx == g_hostDmaStatus.fifoTail.autoDmaRx)
		return 1;

	if (g_hostDmaStatus.fifoHead.autoDmaRx < tailIndex)
	{
		if (g_hostDmaStatus.fifoTail.autoDmaRx < tailIndex)
		{
			if (g_hostDmaStatus.fifoTail.autoDmaRx > g_hostDmaStatus.fifoHead.autoDmaRx)
				return 1;
			else if (g_hostDmaAssistStatus.autoDmaRxOverFlowCnt != (tailAssistIndex + 1))
				return 1;
		}
		else if (g_hostDmaAssistStatus.autoDmaRxOverFlowCnt != tailAssistIndex)
			return 1;
	}
	else if (g_hostDmaStatus.fifoHead.autoDmaRx == tailIndex)
		return 1;
	else
	{
		if (g_hostDmaStatus.fifoTail.autoDmaRx < tailIndex)
			return 1;
		else if (g_hostDmaStatus.fifoTail.autoDmaRx > g_hostDmaStatus.fifoHead.autoDmaRx)
			return 1;
		else if (g_hostDmaAssistStatus.autoDmaRxOverFlowCnt != tailAssistIndex)
			return 1;
	}
	return 0;
}
