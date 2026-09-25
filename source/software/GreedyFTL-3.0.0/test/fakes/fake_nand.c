#include "fake_nand.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "request_schedule.h"

#define FAKE_NAND_MAX_DIES (8 * NSC_MAX_WAYS)
#define FAKE_NAND_BUCKETS 65536u
#define FAKE_NAND_MAX_BAD_BLOCKS 256u
#define FAKE_NAND_MAX_READ_FAILURES 64u

typedef struct FakeNandPage
{
	struct FakeNandPage *next;
	unsigned int die;
	unsigned int rowAddr;
	unsigned char row[FAKE_NAND_ROW_BYTES];
} FakeNandPage;

typedef struct
{
	unsigned int die;
	unsigned int phyBlock;
} FakeNandBlockRef;

typedef struct
{
	unsigned int die;
	unsigned int rowAddr;
} FakeNandRowRef;

static FakeNandPage *buckets[FAKE_NAND_BUCKETS];
static unsigned int programmedPages;
static FakeNandDieStats dieStats[FAKE_NAND_MAX_DIES];
static FakeNandBlockRef badBlocks[FAKE_NAND_MAX_BAD_BLOCKS];
static unsigned int badBlockCount;
static FakeNandRowRef readFailures[FAKE_NAND_MAX_READ_FAILURES];
static unsigned int readFailureCount;

static unsigned int DieIndex(unsigned int ch, unsigned int way)
{
	unsigned int die = ch * NSC_MAX_WAYS + way;
	if (die >= FAKE_NAND_MAX_DIES)
	{
		fprintf(stderr, "fake_nand: die out of range (ch %u way %u)\n", ch, way);
		abort();
	}
	return die;
}

static unsigned int Bucket(unsigned int die, unsigned int rowAddr)
{
	return ((die << 22) ^ rowAddr) * 2654435761u % FAKE_NAND_BUCKETS;
}

static FakeNandPage *FindPage(unsigned int die, unsigned int rowAddr)
{
	FakeNandPage *p;
	for (p = buckets[Bucket(die, rowAddr)]; p; p = p->next)
		if (p->die == die && p->rowAddr == rowAddr)
			return p;
	return NULL;
}

static void RemovePage(unsigned int die, unsigned int rowAddr)
{
	FakeNandPage **link = &buckets[Bucket(die, rowAddr)];
	while (*link)
	{
		FakeNandPage *p = *link;
		if (p->die == die && p->rowAddr == rowAddr)
		{
			*link = p->next;
			free(p);
			programmedPages--;
			return;
		}
		link = &p->next;
	}
}

void FakeNandReset(void)
{
	unsigned int i;
	for (i = 0; i < FAKE_NAND_BUCKETS; i++)
	{
		FakeNandPage *p = buckets[i];
		while (p)
		{
			FakeNandPage *next = p->next;
			free(p);
			p = next;
		}
		buckets[i] = NULL;
	}
	programmedPages = 0;
	memset(dieStats, 0, sizeof(dieStats));
	badBlockCount = 0;
	readFailureCount = 0;
}

unsigned int FakeNandRowToPhyBlock(unsigned int rowAddr)
{
	unsigned int lun = rowAddr >= LUN_1_BASE_ADDR;
	unsigned int rowInLun = rowAddr - (lun ? LUN_1_BASE_ADDR : LUN_0_BASE_ADDR);
	return lun * TOTAL_BLOCKS_PER_LUN + rowInLun / FAKE_NAND_ROWS_PER_BLOCK;
}

unsigned int FakeNandRowToPage(unsigned int rowAddr)
{
	return rowAddr % FAKE_NAND_ROWS_PER_BLOCK;
}

unsigned int FakeNandPhyBlockToRow(unsigned int phyBlock, unsigned int page)
{
	unsigned int lun = phyBlock / TOTAL_BLOCKS_PER_LUN;
	unsigned int blockInLun = phyBlock % TOTAL_BLOCKS_PER_LUN;
	return (lun ? LUN_1_BASE_ADDR : LUN_0_BASE_ADDR) + blockInLun * FAKE_NAND_ROWS_PER_BLOCK + page;
}

int FakeNandIsProgrammed(unsigned int ch, unsigned int way, unsigned int rowAddr)
{
	return FindPage(DieIndex(ch, way), rowAddr) != NULL;
}

void FakeNandReadRow(unsigned int ch, unsigned int way, unsigned int rowAddr, unsigned char *row)
{
	FakeNandPage *p = FindPage(DieIndex(ch, way), rowAddr);
	if (p)
		memcpy(row, p->row, FAKE_NAND_ROW_BYTES);
	else
		memset(row, 0xFF, FAKE_NAND_ROW_BYTES);
}

int FakeNandProgramRow(unsigned int ch, unsigned int way, unsigned int rowAddr, const unsigned char *row)
{
	unsigned int die = DieIndex(ch, way);
	FakeNandPage *p;

	if (FakeNandIsBadBlock(ch, way, FakeNandRowToPhyBlock(rowAddr)))
		return 1;

	if (FindPage(die, rowAddr))
	{
		fprintf(stderr, "fake_nand: program of already-programmed row 0x%08x (ch %u way %u) without erase\n",
		        rowAddr, ch, way);
		abort();
	}

	p = (FakeNandPage *)malloc(sizeof(FakeNandPage));
	if (!p)
	{
		fprintf(stderr, "fake_nand: out of memory\n");
		abort();
	}
	p->die = die;
	p->rowAddr = rowAddr;
	p->next = buckets[Bucket(die, rowAddr)];
	buckets[Bucket(die, rowAddr)] = p;
	programmedPages++;
	memcpy(p->row, row, FAKE_NAND_ROW_BYTES);
	return 0;
}

int FakeNandEraseBlock(unsigned int ch, unsigned int way, unsigned int rowAddr)
{
	unsigned int die = DieIndex(ch, way);
	unsigned int phyBlock = FakeNandRowToPhyBlock(rowAddr);
	unsigned int firstRow = FakeNandPhyBlockToRow(phyBlock, 0);
	unsigned int page;

	if (FakeNandIsBadBlock(ch, way, phyBlock))
		return 1;

	for (page = 0; page < FAKE_NAND_ROWS_PER_BLOCK; page++)
		RemovePage(die, firstRow + page);
	return 0;
}

void FakeNandMarkBadBlock(unsigned int ch, unsigned int way, unsigned int phyBlock)
{
	unsigned int die = DieIndex(ch, way);
	unsigned int rowAddr = FakeNandPhyBlockToRow(phyBlock, BAD_BLOCK_MARK_PAGE0);
	unsigned char row[FAKE_NAND_ROW_BYTES];

	if (FakeNandIsBadBlock(ch, way, phyBlock))
		return;
	if (badBlockCount >= FAKE_NAND_MAX_BAD_BLOCKS)
	{
		fprintf(stderr, "fake_nand: bad block list full\n");
		abort();
	}

	/* The marker row may already hold data; overwrite it in place rather than
	   going through the erase-before-program check for ordinary writes. */
	FakeNandReadRow(ch, way, rowAddr, row);
	RemovePage(die, rowAddr);
	row[BAD_BLOCK_MARK_BYTE0] = 0x00;
	FakeNandProgramRow(ch, way, rowAddr, row);

	badBlocks[badBlockCount].die = die;
	badBlocks[badBlockCount].phyBlock = phyBlock;
	badBlockCount++;
}

int FakeNandIsBadBlock(unsigned int ch, unsigned int way, unsigned int phyBlock)
{
	unsigned int die = DieIndex(ch, way);
	unsigned int i;
	for (i = 0; i < badBlockCount; i++)
		if (badBlocks[i].die == die && badBlocks[i].phyBlock == phyBlock)
			return 1;
	return 0;
}

unsigned int FakeNandBadBlockCount(void)
{
	return badBlockCount;
}

void FakeNandInjectReadFailure(unsigned int ch, unsigned int way, unsigned int rowAddr)
{
	if (readFailureCount >= FAKE_NAND_MAX_READ_FAILURES)
	{
		fprintf(stderr, "fake_nand: read failure list full\n");
		abort();
	}
	readFailures[readFailureCount].die = DieIndex(ch, way);
	readFailures[readFailureCount].rowAddr = rowAddr;
	readFailureCount++;
}

int FakeNandTakeReadFailure(unsigned int ch, unsigned int way, unsigned int rowAddr)
{
	unsigned int die = DieIndex(ch, way);
	unsigned int i;
	for (i = 0; i < readFailureCount; i++)
		if (readFailures[i].die == die && readFailures[i].rowAddr == rowAddr)
		{
			readFailures[i] = readFailures[readFailureCount - 1];
			readFailureCount--;
			return 1;
		}
	return 0;
}

const FakeNandDieStats *FakeNandStats(unsigned int ch, unsigned int way)
{
	return &dieStats[DieIndex(ch, way)];
}

FakeNandDieStats *FakeNandMutableStats(unsigned int ch, unsigned int way)
{
	return &dieStats[DieIndex(ch, way)];
}

FakeNandDieStats FakeNandTotals(void)
{
	FakeNandDieStats total;
	unsigned int i;
	memset(&total, 0, sizeof(total));
	for (i = 0; i < FAKE_NAND_MAX_DIES; i++)
	{
		total.resets += dieStats[i].resets;
		total.setFeatures += dieStats[i].setFeatures;
		total.readTriggers += dieStats[i].readTriggers;
		total.readTransfers += dieStats[i].readTransfers;
		total.programs += dieStats[i].programs;
		total.erases += dieStats[i].erases;
		total.programFails += dieStats[i].programFails;
		total.eraseFails += dieStats[i].eraseFails;
	}
	return total;
}

unsigned int FakeNandProgrammedPageCount(void)
{
	return programmedPages;
}
