/* Host replacement for nsc_driver.c (HOST_TEST build only).
 *
 * Implements the V2F* Tiger4 NAND controller API on top of fake_nand.c. Operations
 * complete immediately; status is reported the way request_schedule.c expects:
 * completion flags for transfers, status-report words for program/erase/read-trigger. */
#include <string.h>

#include "nsc_driver.h"
#include "ftl_config.h"
#include "fake_nand.h"

#define STATUS_READY_BITS 0x60u
#define STATUS_FAIL_BIT 0x01u

typedef struct
{
	unsigned int pendingRow;
	unsigned int lastOpFailed;
} FakeWayState;

static FakeWayState wayState[8][NSC_MAX_WAYS];

static unsigned int ChannelOf(V2FMCRegisters *dev)
{
	unsigned int ch;
	for (ch = 0; ch < USER_CHANNELS; ch++)
		if (chCtlReg[ch] == dev)
			return ch;
	return 0;
}

static unsigned int RowInfoToRawStatus(unsigned int failed)
{
	/* Bit 0 = report done; remaining bits = NAND status byte (0x60 = ready, bit 0/1 = fail). */
	return ((STATUS_READY_BITS | (failed ? STATUS_FAIL_BIT : 0u)) << 1) | 1u;
}

unsigned int V2FIsControllerBusy(V2FMCRegisters *dev)
{
	(void)dev;
	return 0;
}

void V2FResetSync(V2FMCRegisters *dev, int way)
{
	unsigned int ch = ChannelOf(dev);
	FakeNandMutableStats(ch, way)->resets++;
	wayState[ch][way].lastOpFailed = 0;
}

void V2FSetFeaturesSync(V2FMCRegisters *dev, int way, unsigned int feature0x02, unsigned int feature0x10,
						unsigned int feature0x01, unsigned int payLoadAddr)
{
	(void)feature0x02; (void)feature0x10; (void)feature0x01; (void)payLoadAddr;
	FakeNandMutableStats(ChannelOf(dev), way)->setFeatures++;
}

void V2FGetFeaturesSync(V2FMCRegisters *dev, int way, unsigned int *feature0x01, unsigned int *feature0x02,
						unsigned int *feature0x10, unsigned int *feature0x30)
{
	(void)dev; (void)way;
	*feature0x01 = 0;
	*feature0x02 = 0;
	*feature0x10 = 0;
	*feature0x30 = 0;
}

void V2FReadPageTriggerAsync(V2FMCRegisters *dev, int way, unsigned int rowAddress)
{
	unsigned int ch = ChannelOf(dev);
	FakeNandDieStats *stats = FakeNandMutableStats(ch, way);
	stats->readTriggers++;
	stats->lastReadRow = rowAddress;
	wayState[ch][way].pendingRow = rowAddress;
	wayState[ch][way].lastOpFailed = 0;
}

void V2FReadPageTransferAsync(V2FMCRegisters *dev, int way, void *pageDataBuffer, void *spareDataBuffer,
							  unsigned int *errorInformation, unsigned int *completion, unsigned int rowAddress)
{
	unsigned int ch = ChannelOf(dev);
	unsigned char row[FAKE_NAND_ROW_BYTES];

	FakeNandMutableStats(ch, way)->readTransfers++;
	FakeNandReadRow(ch, way, rowAddress, row);
	memcpy(pageDataBuffer, row, BYTES_PER_DATA_REGION_OF_PAGE);
	memcpy(spareDataBuffer, row + BYTES_PER_DATA_REGION_OF_NAND_ROW, BYTES_PER_SPARE_REGION_OF_PAGE);

	if (FakeNandTakeReadFailure(ch, way, rowAddress))
	{
		errorInformation[0] = 0;
		errorInformation[1] = 0;
	}
	else
	{
		errorInformation[0] = 0x10000000u | 0x01000000u; /* CRC valid, spare chunk valid, 0 worst-chunk errors */
		errorInformation[1] = 0xffffffffu;               /* all page chunks valid */
	}
	*completion = 1;
}

void V2FReadPageTransferRawAsync(V2FMCRegisters *dev, int way, void *pageDataBuffer, unsigned int *completion)
{
	unsigned int ch = ChannelOf(dev);
	FakeNandMutableStats(ch, way)->readTransfers++;
	FakeNandReadRow(ch, way, wayState[ch][way].pendingRow, (unsigned char *)pageDataBuffer);
	*completion = 1;
}

void V2FProgramPageAsync(V2FMCRegisters *dev, int way, unsigned int rowAddress, void *pageDataBuffer, void *spareDataBuffer)
{
	unsigned int ch = ChannelOf(dev);
	FakeNandDieStats *stats = FakeNandMutableStats(ch, way);
	unsigned char row[FAKE_NAND_ROW_BYTES];

	memset(row, 0xFF, sizeof(row));
	memcpy(row, pageDataBuffer, BYTES_PER_DATA_REGION_OF_PAGE);
	memcpy(row + BYTES_PER_DATA_REGION_OF_NAND_ROW, spareDataBuffer, BYTES_PER_SPARE_REGION_OF_PAGE);

	stats->programs++;
	stats->lastProgramRow = rowAddress;
	wayState[ch][way].lastOpFailed = FakeNandProgramRow(ch, way, rowAddress, row) != 0;
	if (wayState[ch][way].lastOpFailed)
		stats->programFails++;
}

void V2FEraseBlockAsync(V2FMCRegisters *dev, int way, unsigned int rowAddress)
{
	unsigned int ch = ChannelOf(dev);
	FakeNandDieStats *stats = FakeNandMutableStats(ch, way);

	stats->erases++;
	stats->lastEraseRow = rowAddress;
	wayState[ch][way].lastOpFailed = FakeNandEraseBlock(ch, way, rowAddress) != 0;
	if (wayState[ch][way].lastOpFailed)
		stats->eraseFails++;
}

void V2FStatusCheckAsync(V2FMCRegisters *dev, int way, unsigned int *statusReport)
{
	unsigned int ch = ChannelOf(dev);
	*statusReport = RowInfoToRawStatus(wayState[ch][way].lastOpFailed);
}

unsigned int V2FStatusCheckSync(V2FMCRegisters *dev, int way)
{
	unsigned int ch = ChannelOf(dev);
	return RowInfoToRawStatus(wayState[ch][way].lastOpFailed) >> 1;
}

unsigned int V2FReadyBusyAsync(V2FMCRegisters *dev)
{
	(void)dev;
	return 0xFFu;
}
