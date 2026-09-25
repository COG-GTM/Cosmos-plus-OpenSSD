/*
 * Fake Tiger4 NAND storage controller.
 *
 * Replaces nsc_driver.c for host builds. Every V2F* call is recorded as a
 * register write (cmdSelect/rowAddress/...) into the fake register map at the
 * channel's base address, and the effect on flash is applied immediately to
 * the in-memory array from fake_nand.h. Completion flags, status reports and
 * ECC error words are written back through the same pointers the hardware DMA
 * engine would use, so request_schedule.c sees the operation finish on its
 * next scheduling pass.
 */

#include "fake_nand.h"

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "fake_regs.h"
#include "address_translation.h"
#include "ftl_config.h"
#include "nsc_driver.h"
#include "request_schedule.h"

#define ROWS_PER_BLOCK   ROWS_PER_MLC_BLOCK
#define ROW_BYTES        BYTES_PER_NAND_ROW
#define BLOCKS_PER_DIE   TOTAL_BLOCKS_PER_DIE
#define MAX_BAD_BLOCKS   256

typedef struct
{
	unsigned char *rows;               /* ROWS_PER_BLOCK * ROW_BYTES, NULL until first program */
	unsigned int eraseCount;
	unsigned int programCount;
} FakeBlock;

typedef struct
{
	FakeBlock blocks[BLOCKS_PER_DIE];
	unsigned int badBlocks[MAX_BAD_BLOCKS];
	unsigned int badBlockCount;
	unsigned int lastOpFailed;
	unsigned int failNextProgram;
	unsigned int failNextErase;
	unsigned int eccWorstChunkErrors;
	unsigned int eccUncorrectable;
	FAKE_NAND_DIE_STATS stats;
} FakeDie;

static FakeDie dies[USER_CHANNELS][USER_WAYS];

static unsigned int ChannelOf(V2FMCRegisters *dev)
{
	unsigned int ch;
	for (ch = 0; ch < USER_CHANNELS; ch++)
		if (chCtlReg[ch] == dev)
			return ch;
	fprintf(stderr, "fake_nsc_driver: unknown controller %p\n", (void *)dev);
	abort();
}

static unsigned int RegAddr(V2FMCRegisters *dev, size_t fieldOffset)
{
	return (unsigned int)(uintptr_t)dev + (unsigned int)fieldOffset;
}

#define WRITE_REG(dev, field, value) fake_reg_write(RegAddr((dev), offsetof(V2FMCRegisters, field)), (value))

static void DecodeRow(unsigned int rowAddr, unsigned int *phyBlock, unsigned int *row)
{
	unsigned int lun = rowAddr / LUN_1_BASE_ADDR;
	unsigned int inLun = rowAddr % LUN_1_BASE_ADDR;
	*phyBlock = lun * TOTAL_BLOCKS_PER_LUN + inLun / ROWS_PER_BLOCK;
	*row = inLun % ROWS_PER_BLOCK;
	if (*phyBlock >= BLOCKS_PER_DIE)
	{
		fprintf(stderr, "fake_nsc_driver: row address 0x%x out of range\n", rowAddr);
		abort();
	}
}

static FakeBlock *BlockAt(unsigned int ch, unsigned int way, unsigned int phyBlock, int create)
{
	FakeBlock *b = &dies[ch][way].blocks[phyBlock];
	if (b->rows == NULL && create)
	{
		b->rows = malloc((size_t)ROWS_PER_BLOCK * ROW_BYTES);
		memset(b->rows, 0xFF, (size_t)ROWS_PER_BLOCK * ROW_BYTES);
	}
	return b;
}

static void ValidateDie(unsigned int ch, unsigned int way)
{
	if (ch >= USER_CHANNELS || way >= USER_WAYS)
	{
		fprintf(stderr, "fake_nand: die ch%u way%u out of range\n", ch, way);
		abort();
	}
}

/* ---- public model API -------------------------------------------------- */

void fake_nand_reset(void)
{
	unsigned int ch, way, blk;
	for (ch = 0; ch < USER_CHANNELS; ch++)
		for (way = 0; way < USER_WAYS; way++)
		{
			for (blk = 0; blk < BLOCKS_PER_DIE; blk++)
				free(dies[ch][way].blocks[blk].rows);
			memset(&dies[ch][way], 0, sizeof(FakeDie));
		}
}

unsigned int fake_nand_row_addr(unsigned int phyBlock, unsigned int row)
{
	unsigned int lun = phyBlock / TOTAL_BLOCKS_PER_LUN;
	unsigned int inLun = phyBlock % TOTAL_BLOCKS_PER_LUN;
	return (lun ? LUN_1_BASE_ADDR : LUN_0_BASE_ADDR) + inLun * ROWS_PER_BLOCK + row;
}

unsigned char *fake_nand_row(unsigned int ch, unsigned int way, unsigned int rowAddr)
{
	unsigned int phyBlock, row;
	ValidateDie(ch, way);
	DecodeRow(rowAddr, &phyBlock, &row);
	return BlockAt(ch, way, phyBlock, 1)->rows + (size_t)row * ROW_BYTES;
}

int fake_nand_row_is_programmed(unsigned int ch, unsigned int way, unsigned int rowAddr)
{
	unsigned int phyBlock, row, i;
	const unsigned char *p;
	ValidateDie(ch, way);
	DecodeRow(rowAddr, &phyBlock, &row);
	if (BlockAt(ch, way, phyBlock, 0)->rows == NULL)
		return 0;
	p = dies[ch][way].blocks[phyBlock].rows + (size_t)row * ROW_BYTES;
	for (i = 0; i < ROW_BYTES; i++)
		if (p[i] != 0xFF)
			return 1;
	return 0;
}

void fake_nand_mark_bad(unsigned int ch, unsigned int way, unsigned int phyBlock)
{
	FakeDie *d;
	ValidateDie(ch, way);
	d = &dies[ch][way];
	if (fake_nand_is_bad(ch, way, phyBlock))
		return;
	if (d->badBlockCount >= MAX_BAD_BLOCKS)
	{
		fprintf(stderr, "fake_nand: too many bad blocks on ch%u way%u\n", ch, way);
		abort();
	}
	d->badBlocks[d->badBlockCount++] = phyBlock;
	fake_nand_row(ch, way, fake_nand_row_addr(phyBlock, BAD_BLOCK_MARK_PAGE0))[BAD_BLOCK_MARK_BYTE0] = 0x00;
	fake_nand_row(ch, way, fake_nand_row_addr(phyBlock, BAD_BLOCK_MARK_PAGE1))[BAD_BLOCK_MARK_BYTE1] = 0x00;
}

int fake_nand_is_bad(unsigned int ch, unsigned int way, unsigned int phyBlock)
{
	unsigned int i;
	ValidateDie(ch, way);
	for (i = 0; i < dies[ch][way].badBlockCount; i++)
		if (dies[ch][way].badBlocks[i] == phyBlock)
			return 1;
	return 0;
}

unsigned int fake_nand_bad_block_count(unsigned int ch, unsigned int way)
{
	ValidateDie(ch, way);
	return dies[ch][way].badBlockCount;
}

void fake_nand_preload_bbt(unsigned int ch, unsigned int way, unsigned int bbtPhyBlock)
{
	/* Layout mirrors ReadBadBlockTable(): one byte per physical block, starting at the
	 * first LSB page after the bad-block-mark page, BYTES_PER_DATA_REGION_OF_PAGE per page. */
	unsigned int blk, page = 0, vpage = PlsbPage2VpageTranslation(START_PAGE_NO_OF_BAD_BLOCK_TABLE_BLOCK);
	unsigned char *row = NULL;
	ValidateDie(ch, way);
	for (blk = 0; blk < BLOCKS_PER_DIE; blk++)
	{
		unsigned int offset = blk % BYTES_PER_DATA_REGION_OF_PAGE;
		if (offset == 0)
		{
			row = fake_nand_row(ch, way, fake_nand_row_addr(bbtPhyBlock, Vpage2PlsbPageTranslation(vpage + page)));
			page++;
		}
		row[offset] = fake_nand_is_bad(ch, way, blk) ? BLOCK_STATE_BAD : BLOCK_STATE_NORMAL;
	}
}

void fake_nand_fail_next_program(unsigned int ch, unsigned int way)
{
	ValidateDie(ch, way);
	dies[ch][way].failNextProgram = 1;
}

void fake_nand_fail_next_erase(unsigned int ch, unsigned int way)
{
	ValidateDie(ch, way);
	dies[ch][way].failNextErase = 1;
}

void fake_nand_set_ecc_result(unsigned int ch, unsigned int way, unsigned int worstChunkErrors, int uncorrectable)
{
	ValidateDie(ch, way);
	dies[ch][way].eccWorstChunkErrors = worstChunkErrors;
	dies[ch][way].eccUncorrectable = uncorrectable ? 1 : 0;
}

FAKE_NAND_DIE_STATS fake_nand_stats(unsigned int ch, unsigned int way)
{
	ValidateDie(ch, way);
	return dies[ch][way].stats;
}

unsigned int fake_nand_block_erase_count(unsigned int ch, unsigned int way, unsigned int phyBlock)
{
	ValidateDie(ch, way);
	return dies[ch][way].blocks[phyBlock].eraseCount;
}

unsigned int fake_nand_block_program_count(unsigned int ch, unsigned int way, unsigned int phyBlock)
{
	ValidateDie(ch, way);
	return dies[ch][way].blocks[phyBlock].programCount;
}

/* ---- V2F controller API ------------------------------------------------ */

unsigned int V2FIsControllerBusy(V2FMCRegisters *dev)
{
	(void)ChannelOf(dev);
	return 0;
}

void V2FResetSync(V2FMCRegisters *dev, int way)
{
	unsigned int ch = ChannelOf(dev);
	WRITE_REG(dev, waySelection, (unsigned int)way);
	WRITE_REG(dev, cmdSelect, V2FCommand_Reset);
	dies[ch][way].lastOpFailed = 0;
	dies[ch][way].stats.resets++;
}

void V2FSetFeaturesSync(V2FMCRegisters *dev, int way, unsigned int feature0x02, unsigned int feature0x10, unsigned int feature0x01, unsigned int payLoadAddr)
{
	unsigned int ch = ChannelOf(dev);
	unsigned int *payload = (unsigned int *)(uintptr_t)payLoadAddr;
	payload[0] = feature0x02;
	payload[1] = feature0x10;
	payload[2] = feature0x01;
	payload[3] = 0;
	WRITE_REG(dev, waySelection, (unsigned int)way);
	WRITE_REG(dev, userData, payLoadAddr);
	WRITE_REG(dev, cmdSelect, V2FCommand_SetFeatures);
	dies[ch][way].stats.setFeatures++;
}

void V2FGetFeaturesSync(V2FMCRegisters *dev, int way, unsigned int *feature0x01, unsigned int *feature0x02, unsigned int *feature0x10, unsigned int *feature0x30)
{
	(void)ChannelOf(dev);
	WRITE_REG(dev, waySelection, (unsigned int)way);
	WRITE_REG(dev, cmdSelect, V2FCommand_GetFeatures);
	*feature0x01 = 0x20;
	*feature0x02 = 0x06;
	*feature0x10 = 0x08;
	*feature0x30 = 0;
}

void V2FReadPageTriggerAsync(V2FMCRegisters *dev, int way, unsigned int rowAddress)
{
	unsigned int ch = ChannelOf(dev);
	unsigned int phyBlock, row;
	DecodeRow(rowAddress, &phyBlock, &row);
	WRITE_REG(dev, waySelection, (unsigned int)way);
	WRITE_REG(dev, rowAddress, rowAddress);
	WRITE_REG(dev, cmdSelect, V2FCommand_ReadPageTrigger);
	dies[ch][way].lastOpFailed = 0;
	dies[ch][way].stats.readTriggers++;
}

void V2FReadPageTransferAsync(V2FMCRegisters *dev, int way, void *pageDataBuffer, void *spareDataBuffer, unsigned int *errorInformation, unsigned int *completion, unsigned int rowAddress)
{
	unsigned int ch = ChannelOf(dev);
	unsigned int phyBlock, row;
	FakeDie *d = &dies[ch][way];
	FakeBlock *b;

	DecodeRow(rowAddress, &phyBlock, &row);
	WRITE_REG(dev, waySelection, (unsigned int)way);
	WRITE_REG(dev, dataAddress, (unsigned int)(uintptr_t)pageDataBuffer);
	WRITE_REG(dev, spareAddress, (unsigned int)(uintptr_t)spareDataBuffer);
	WRITE_REG(dev, errorCountAddress, (unsigned int)(uintptr_t)errorInformation);
	WRITE_REG(dev, completionAddress, (unsigned int)(uintptr_t)completion);
	WRITE_REG(dev, rowAddress, rowAddress);
	WRITE_REG(dev, cmdSelect, V2FCommand_ReadPageTransfer);

	b = BlockAt(ch, way, phyBlock, 0);
	if (b->rows != NULL)
	{
		const unsigned char *src = b->rows + (size_t)row * ROW_BYTES;
		memcpy(pageDataBuffer, src, BYTES_PER_DATA_REGION_OF_PAGE);
		memcpy(spareDataBuffer, src + BYTES_PER_DATA_REGION_OF_PAGE, BYTES_PER_SPARE_REGION_OF_PAGE);
	}
	else
	{
		memset(pageDataBuffer, 0xFF, BYTES_PER_DATA_REGION_OF_PAGE);
		memset(spareDataBuffer, 0xFF, BYTES_PER_SPARE_REGION_OF_PAGE);
	}

	/* errorInformation[0]: CRC valid | spare chunk valid | worst chunk error count. */
	errorInformation[0] = 0x10000000u | 0x01000000u | ((d->eccWorstChunkErrors & 0xFF) << 16);
	/* errorInformation[1]: 0xffffffff means every page chunk decoded. */
	errorInformation[1] = d->eccUncorrectable ? 0x00000000u : 0xFFFFFFFFu;
	*completion = 1;
	d->stats.readTransfers++;
}

void V2FReadPageTransferRawAsync(V2FMCRegisters *dev, int way, void *pageDataBuffer, unsigned int *completion)
{
	unsigned int ch = ChannelOf(dev);
	unsigned int rowAddress = fake_reg_read(RegAddr(dev, offsetof(V2FMCRegisters, rowAddress)));
	unsigned int phyBlock, row;
	FakeBlock *b;

	/* The raw transfer reuses the row latched by the preceding ReadPageTrigger. */
	DecodeRow(rowAddress, &phyBlock, &row);
	WRITE_REG(dev, waySelection, (unsigned int)way);
	WRITE_REG(dev, dataAddress, (unsigned int)(uintptr_t)pageDataBuffer);
	WRITE_REG(dev, completionAddress, (unsigned int)(uintptr_t)completion);
	WRITE_REG(dev, cmdSelect, V2FCommand_ReadPageTransferRaw);

	b = BlockAt(ch, way, phyBlock, 0);
	if (b->rows != NULL)
		memcpy(pageDataBuffer, b->rows + (size_t)row * ROW_BYTES, ROW_BYTES);
	else
		memset(pageDataBuffer, 0xFF, ROW_BYTES);
	*completion = 1;
	dies[ch][way].stats.rawReadTransfers++;
}

void V2FProgramPageAsync(V2FMCRegisters *dev, int way, unsigned int rowAddress, void *pageDataBuffer, void *spareDataBuffer)
{
	unsigned int ch = ChannelOf(dev);
	unsigned int phyBlock, row;
	FakeDie *d = &dies[ch][way];
	FakeBlock *b;
	unsigned char *dst;

	DecodeRow(rowAddress, &phyBlock, &row);
	WRITE_REG(dev, waySelection, (unsigned int)way);
	WRITE_REG(dev, dataAddress, (unsigned int)(uintptr_t)pageDataBuffer);
	WRITE_REG(dev, spareAddress, (unsigned int)(uintptr_t)spareDataBuffer);
	WRITE_REG(dev, rowAddress, rowAddress);
	WRITE_REG(dev, cmdSelect, V2FCommand_ProgramPage);

	d->stats.programs++;
	if (d->failNextProgram || fake_nand_is_bad(ch, way, phyBlock))
	{
		d->failNextProgram = 0;
		d->lastOpFailed = 1;
		return;
	}

	b = BlockAt(ch, way, phyBlock, 1);
	dst = b->rows + (size_t)row * ROW_BYTES;
	memcpy(dst, pageDataBuffer, BYTES_PER_DATA_REGION_OF_PAGE);
	memcpy(dst + BYTES_PER_DATA_REGION_OF_PAGE, spareDataBuffer, BYTES_PER_SPARE_REGION_OF_PAGE);
	b->programCount++;
	d->lastOpFailed = 0;
}

void V2FEraseBlockAsync(V2FMCRegisters *dev, int way, unsigned int rowAddress)
{
	unsigned int ch = ChannelOf(dev);
	unsigned int phyBlock, row;
	FakeDie *d = &dies[ch][way];
	FakeBlock *b;

	DecodeRow(rowAddress, &phyBlock, &row);
	WRITE_REG(dev, waySelection, (unsigned int)way);
	WRITE_REG(dev, rowAddress, rowAddress);
	WRITE_REG(dev, cmdSelect, V2FCommand_BlockErase);

	d->stats.erases++;
	b = &d->blocks[phyBlock];
	b->eraseCount++;
	if (d->failNextErase || fake_nand_is_bad(ch, way, phyBlock))
	{
		d->failNextErase = 0;
		d->lastOpFailed = 1;
		return;
	}
	if (b->rows != NULL)
	{
		/* Keep the block allocated but return it to the erased state. */
		free(b->rows);
		b->rows = NULL;
	}
	d->lastOpFailed = 0;
}

static unsigned int StatusWord(FakeDie *d)
{
	/* Hardware format: bit0 = report done, then NAND status byte (0x60 = ready, bits 0-1 = fail). */
	unsigned int status = 0x60 | (d->lastOpFailed ? 0x1 : 0x0);
	return 1u | (status << 1);
}

void V2FStatusCheckAsync(V2FMCRegisters *dev, int way, unsigned int *statusReport)
{
	unsigned int ch = ChannelOf(dev);
	WRITE_REG(dev, waySelection, (unsigned int)way);
	WRITE_REG(dev, completionAddress, (unsigned int)(uintptr_t)statusReport);
	WRITE_REG(dev, cmdSelect, V2FCommand_StatusCheck);
	*statusReport = StatusWord(&dies[ch][way]);
	dies[ch][way].stats.statusChecks++;
}

unsigned int V2FStatusCheckSync(V2FMCRegisters *dev, int way)
{
	unsigned int status;
	V2FStatusCheckAsync(dev, way, &status);
	return status >> 1;
}

unsigned int V2FReadyBusyAsync(V2FMCRegisters *dev)
{
	(void)ChannelOf(dev);
	return (1u << USER_WAYS) - 1u;
}
