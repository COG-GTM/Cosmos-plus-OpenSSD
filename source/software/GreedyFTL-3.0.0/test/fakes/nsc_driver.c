/*
 * Fake Tiger4 NAND storage controller driver (replaces the production nsc_driver.c
 * in the host build).
 *
 * The production driver truncates buffer pointers to 32-bit register fields, which
 * cannot work in a 64-bit process. This implementation keeps the exact V2F* interface
 * and serves every command synchronously from an in-memory flash model:
 *  - read trigger / status check / read transfer / program / erase / reset / set features
 *  - completion flags, status reports and ECC error information are written into the
 *    same tables the hardware would DMA into, so request_schedule.c runs unmodified
 *  - failures (program, erase, ECC) can be injected per die to reach the firmware's
 *    grown-bad-block handling
 */
#include <assert.h>
#include <stdlib.h>
#include <string.h>

#include "ftl_config.h"
#include "address_translation.h"
#include "request_schedule.h"
#include "nsc_driver.h"
#include "fake_nand.h"

#define ROW_ADDR_BITS			22			/* LUN_1_BASE_ADDR is 0x00200000 */
#define STATUS_OK				0x60		/* V2FRequestComplete() bits set, fail bits clear */
#define STATUS_FAIL				0x63
#define ECC_INFO_PASS			0x11000000	/* CRC valid + spare chunk valid, worst chunk error count 0 */
#define ECC_INFO_FAIL			0x00000000
#define PAGE_CHUNKS_VALID		0xffffffff

typedef struct
{
	unsigned int key;
	unsigned char used;
	unsigned char data[BYTES_PER_DATA_REGION_OF_PAGE];
	unsigned char spare[BYTES_PER_SPARE_REGION_OF_PAGE];
} FAKE_PAGE;

typedef struct
{
	unsigned int *keys;
	FAKE_PAGE **pages;
	unsigned int capacity;
	unsigned int count;
} PAGE_MAP;

typedef struct
{
	unsigned int lastStatus;
	unsigned int triggeredRow;
	unsigned int programFailsPending;
	unsigned int eraseFailsPending;
	unsigned int eccFailsPending;
} DIE_MODEL;

static PAGE_MAP pageMap;
static DIE_MODEL dies[USER_CHANNELS][USER_WAYS];
static unsigned char factoryBad[USER_CHANNELS][USER_WAYS][TOTAL_BLOCKS_PER_DIE];
static FAKE_NAND_STATS stats;

static unsigned int channelOf(V2FMCRegisters *dev)
{
	unsigned int chNo;

	for (chNo = 0; chNo < USER_CHANNELS; chNo++)
		if (chCtlReg[chNo] == dev)
			return chNo;
	assert(!"unknown NAND controller instance");
	return 0;
}

static unsigned int pageKey(unsigned int chNo, unsigned int wayNo, unsigned int rowAddr)
{
	return ((chNo * USER_WAYS + wayNo) << ROW_ADDR_BITS) | (rowAddr & ((1u << ROW_ADDR_BITS) - 1));
}

static unsigned int hashKey(unsigned int key, unsigned int capacity)
{
	return (key * 2654435761u) & (capacity - 1);
}

static void mapGrow(void);

static FAKE_PAGE **mapSlot(unsigned int key, int create)
{
	unsigned int idx;

	if (pageMap.capacity == 0 || (create && pageMap.count * 2 >= pageMap.capacity))
		mapGrow();

	idx = hashKey(key, pageMap.capacity);
	for (;;)
	{
		if (pageMap.pages[idx] == NULL)
		{
			if (!create)
				return NULL;
			pageMap.keys[idx] = key;
			pageMap.count++;
			return &pageMap.pages[idx];
		}
		if (pageMap.keys[idx] == key)
			return &pageMap.pages[idx];
		idx = (idx + 1) & (pageMap.capacity - 1);
	}
}

static void mapGrow(void)
{
	PAGE_MAP old = pageMap;
	unsigned int i;

	pageMap.capacity = old.capacity ? old.capacity * 2 : 1024;
	pageMap.count = 0;
	pageMap.keys = calloc(pageMap.capacity, sizeof(unsigned int));
	pageMap.pages = calloc(pageMap.capacity, sizeof(FAKE_PAGE *));
	assert(pageMap.keys && pageMap.pages);

	for (i = 0; i < old.capacity; i++)
		if (old.pages[i])
			*mapSlot(old.keys[i], 1) = old.pages[i];

	free(old.keys);
	free(old.pages);
}

static FAKE_PAGE *findPage(unsigned int chNo, unsigned int wayNo, unsigned int rowAddr)
{
	FAKE_PAGE **slot = mapSlot(pageKey(chNo, wayNo, rowAddr), 0);
	return slot ? *slot : NULL;
}

static void removePage(unsigned int chNo, unsigned int wayNo, unsigned int rowAddr)
{
	unsigned int key = pageKey(chNo, wayNo, rowAddr);
	unsigned int idx, next;

	if (pageMap.capacity == 0)
		return;

	idx = hashKey(key, pageMap.capacity);
	while (pageMap.pages[idx] && pageMap.keys[idx] != key)
		idx = (idx + 1) & (pageMap.capacity - 1);
	if (!pageMap.pages[idx])
		return;

	free(pageMap.pages[idx]);
	pageMap.pages[idx] = NULL;
	pageMap.count--;

	/* Re-insert the rest of the probe run so linear probing stays consistent. */
	next = (idx + 1) & (pageMap.capacity - 1);
	while (pageMap.pages[next])
	{
		FAKE_PAGE *moved = pageMap.pages[next];
		unsigned int movedKey = pageMap.keys[next];
		pageMap.pages[next] = NULL;
		pageMap.count--;
		*mapSlot(movedKey, 1) = moved;
		next = (next + 1) & (pageMap.capacity - 1);
	}
}

void fake_nand_reset(void)
{
	unsigned int i;

	for (i = 0; i < pageMap.capacity; i++)
		free(pageMap.pages[i]);
	free(pageMap.keys);
	free(pageMap.pages);
	memset(&pageMap, 0, sizeof(pageMap));
	memset(dies, 0, sizeof(dies));
	memset(factoryBad, 0, sizeof(factoryBad));
	memset(&stats, 0, sizeof(stats));
}

void fake_nand_reset_stats(void)
{
	memset(&stats, 0, sizeof(stats));
}

const FAKE_NAND_STATS *fake_nand_stats(void)
{
	return &stats;
}

unsigned int fake_nand_row_to_phy_block(unsigned int rowAddr)
{
	return ((rowAddr % LUN_1_BASE_ADDR) / PAGES_PER_MLC_BLOCK) + ((rowAddr / LUN_1_BASE_ADDR) * TOTAL_BLOCKS_PER_LUN);
}

unsigned int fake_nand_row_to_page(unsigned int rowAddr)
{
	return rowAddr % PAGES_PER_MLC_BLOCK;
}

unsigned int fake_nand_phy_block_to_row(unsigned int phyBlockNo, unsigned int pageNo)
{
	unsigned int lun = phyBlockNo / TOTAL_BLOCKS_PER_LUN;
	unsigned int blockInLun = phyBlockNo % TOTAL_BLOCKS_PER_LUN;
	return (lun ? LUN_1_BASE_ADDR : LUN_0_BASE_ADDR) + blockInLun * PAGES_PER_MLC_BLOCK + pageNo;
}

void fake_nand_mark_factory_bad(unsigned int chNo, unsigned int wayNo, unsigned int phyBlockNo)
{
	assert(chNo < USER_CHANNELS && wayNo < USER_WAYS && phyBlockNo < TOTAL_BLOCKS_PER_DIE);
	factoryBad[chNo][wayNo][phyBlockNo] = 1;
}

unsigned int fake_nand_is_factory_bad(unsigned int chNo, unsigned int wayNo, unsigned int phyBlockNo)
{
	return factoryBad[chNo][wayNo][phyBlockNo];
}

void fake_nand_inject_program_fail(unsigned int chNo, unsigned int wayNo, unsigned int count)
{
	dies[chNo][wayNo].programFailsPending = count;
}

void fake_nand_inject_erase_fail(unsigned int chNo, unsigned int wayNo, unsigned int count)
{
	dies[chNo][wayNo].eraseFailsPending = count;
}

void fake_nand_inject_ecc_fail(unsigned int chNo, unsigned int wayNo, unsigned int count)
{
	dies[chNo][wayNo].eccFailsPending = count;
}

const unsigned char *fake_nand_page_data(unsigned int chNo, unsigned int wayNo, unsigned int rowAddr)
{
	FAKE_PAGE *page = findPage(chNo, wayNo, rowAddr);
	return page ? page->data : NULL;
}

const unsigned char *fake_nand_page_spare(unsigned int chNo, unsigned int wayNo, unsigned int rowAddr)
{
	FAKE_PAGE *page = findPage(chNo, wayNo, rowAddr);
	return page ? page->spare : NULL;
}

unsigned int fake_nand_written_page_count(void)
{
	return pageMap.count;
}

static int rowCarriesBadMark(unsigned int chNo, unsigned int wayNo, unsigned int rowAddr)
{
	unsigned int pageNo = fake_nand_row_to_page(rowAddr);

	if (!factoryBad[chNo][wayNo][fake_nand_row_to_phy_block(rowAddr)])
		return 0;
	return pageNo == BAD_BLOCK_MARK_PAGE0 || pageNo == BAD_BLOCK_MARK_PAGE1;
}

/* ---- V2F driver interface ---- */

unsigned int V2FIsControllerBusy(V2FMCRegisters *dev)
{
	(void)dev;
	return 0;
}

void V2FResetSync(V2FMCRegisters *dev, int way)
{
	dies[channelOf(dev)][way].lastStatus = STATUS_OK;
	stats.resets++;
}

void V2FSetFeaturesSync(V2FMCRegisters *dev, int way, unsigned int feature0x02, unsigned int feature0x10, unsigned int feature0x01, unsigned int payLoadAddr)
{
	(void)feature0x02; (void)feature0x10; (void)feature0x01; (void)payLoadAddr;
	dies[channelOf(dev)][way].lastStatus = STATUS_OK;
	stats.setFeatures++;
}

void V2FGetFeaturesSync(V2FMCRegisters *dev, int way, unsigned int *feature0x01, unsigned int *feature0x02, unsigned int *feature0x10, unsigned int *feature0x30)
{
	(void)dev; (void)way;
	*feature0x01 = 0;
	*feature0x02 = 0;
	*feature0x10 = 0;
	*feature0x30 = 0;
}

void V2FReadPageTriggerAsync(V2FMCRegisters *dev, int way, unsigned int rowAddress)
{
	DIE_MODEL *die = &dies[channelOf(dev)][way];

	die->triggeredRow = rowAddress;
	die->lastStatus = STATUS_OK;
	stats.readTriggers++;
}

static void fillFromPage(unsigned int chNo, unsigned int wayNo, unsigned int rowAddr, unsigned char *dataBuf, unsigned int dataLen, unsigned char *spareBuf, unsigned int spareLen)
{
	FAKE_PAGE *page = findPage(chNo, wayNo, rowAddr);

	if (dataBuf)
	{
		memset(dataBuf, CLEAN_DATA_IN_BYTE, dataLen);
		if (page)
			memcpy(dataBuf, page->data, dataLen < sizeof(page->data) ? dataLen : sizeof(page->data));
	}
	if (spareBuf)
	{
		memset(spareBuf, CLEAN_DATA_IN_BYTE, spareLen);
		if (page)
			memcpy(spareBuf, page->spare, spareLen < sizeof(page->spare) ? spareLen : sizeof(page->spare));
	}
}

void V2FReadPageTransferAsync(V2FMCRegisters *dev, int way, void *pageDataBuffer, void *spareDataBuffer, unsigned int *errorInformation, unsigned int *completion, unsigned int rowAddress)
{
	unsigned int chNo = channelOf(dev);
	DIE_MODEL *die = &dies[chNo][way];

	fillFromPage(chNo, way, rowAddress, pageDataBuffer, BYTES_PER_DATA_REGION_OF_PAGE, spareDataBuffer, BYTES_PER_SPARE_REGION_OF_PAGE);

	memset(errorInformation, 0, ERROR_INFO_WORD_COUNT * sizeof(unsigned int));
	if (die->eccFailsPending)
	{
		die->eccFailsPending--;
		errorInformation[0] = ECC_INFO_FAIL;
		errorInformation[1] = 0;
	}
	else
	{
		errorInformation[0] = ECC_INFO_PASS;
		errorInformation[1] = PAGE_CHUNKS_VALID;
	}
	*completion = 1;
	stats.readTransfers++;
}

void V2FReadPageTransferRawAsync(V2FMCRegisters *dev, int way, void *pageDataBuffer, unsigned int *completion)
{
	unsigned int chNo = channelOf(dev);
	DIE_MODEL *die = &dies[chNo][way];
	unsigned char *row = pageDataBuffer;

	fillFromPage(chNo, way, die->triggeredRow, row, BYTES_PER_DATA_REGION_OF_NAND_ROW, row + BYTES_PER_DATA_REGION_OF_NAND_ROW, BYTES_PER_SPARE_REGION_OF_NAND_ROW);

	if (rowCarriesBadMark(chNo, way, die->triggeredRow))
	{
		row[BAD_BLOCK_MARK_BYTE0] = 0x00;
		row[BAD_BLOCK_MARK_BYTE1] = 0x00;
	}
	*completion = 1;
	stats.rawReadTransfers++;
}

void V2FProgramPageAsync(V2FMCRegisters *dev, int way, unsigned int rowAddress, void *pageDataBuffer, void *spareDataBuffer)
{
	unsigned int chNo = channelOf(dev);
	DIE_MODEL *die = &dies[chNo][way];
	FAKE_PAGE **slot;

	stats.programs++;
	if (die->programFailsPending)
	{
		die->programFailsPending--;
		die->lastStatus = STATUS_FAIL;
		return;
	}

	slot = mapSlot(pageKey(chNo, way, rowAddress), 1);
	if (*slot == NULL)
	{
		*slot = malloc(sizeof(FAKE_PAGE));
		assert(*slot);
	}
	memcpy((*slot)->data, pageDataBuffer, sizeof((*slot)->data));
	if (spareDataBuffer)
		memcpy((*slot)->spare, spareDataBuffer, sizeof((*slot)->spare));
	else
		memset((*slot)->spare, CLEAN_DATA_IN_BYTE, sizeof((*slot)->spare));
	die->lastStatus = STATUS_OK;
}

void V2FEraseBlockAsync(V2FMCRegisters *dev, int way, unsigned int rowAddress)
{
	unsigned int chNo = channelOf(dev);
	DIE_MODEL *die = &dies[chNo][way];
	unsigned int firstRow = rowAddress - fake_nand_row_to_page(rowAddress);
	unsigned int pageNo;

	stats.erases++;
	if (die->eraseFailsPending)
	{
		die->eraseFailsPending--;
		die->lastStatus = STATUS_FAIL;
		return;
	}

	for (pageNo = 0; pageNo < PAGES_PER_MLC_BLOCK; pageNo++)
		removePage(chNo, way, firstRow + pageNo);
	die->lastStatus = STATUS_OK;
}

void V2FStatusCheckAsync(V2FMCRegisters *dev, int way, unsigned int *statusReport)
{
	*statusReport = (dies[channelOf(dev)][way].lastStatus << 1) | 1;
	stats.statusChecks++;
}

unsigned int V2FStatusCheckSync(V2FMCRegisters *dev, int way)
{
	return dies[channelOf(dev)][way].lastStatus;
}

unsigned int V2FReadyBusyAsync(V2FMCRegisters *dev)
{
	(void)dev;
	return (1u << USER_WAYS) - 1;
}
