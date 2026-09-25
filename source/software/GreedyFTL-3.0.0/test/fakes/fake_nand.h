/* In-memory NAND flash model (HOST_TEST build only).
 *
 * Pages are addressed by (channel, way, row) exactly as the Tiger4 NAND controller
 * sees them. Unprogrammed pages read back as 0xFF. A bad-block list makes program
 * and erase fail and stamps the factory bad-block mark so the FTL's scan finds it. */
#ifndef FAKE_NAND_H
#define FAKE_NAND_H

#include "ftl_config.h"

#ifdef __cplusplus
extern "C" {
#endif

#define FAKE_NAND_ROW_BYTES BYTES_PER_NAND_ROW
#define FAKE_NAND_ROWS_PER_BLOCK ROWS_PER_MLC_BLOCK

typedef struct
{
	unsigned int resets;
	unsigned int setFeatures;
	unsigned int readTriggers;
	unsigned int readTransfers;
	unsigned int programs;
	unsigned int erases;
	unsigned int programFails;
	unsigned int eraseFails;
	unsigned int lastReadRow;
	unsigned int lastProgramRow;
	unsigned int lastEraseRow;
} FakeNandDieStats;

void FakeNandReset(void);

/* Row/block helpers mirroring GenerateNandRowAddr(): lun 1 rows start at LUN_1_BASE_ADDR. */
unsigned int FakeNandRowToPhyBlock(unsigned int rowAddr);
unsigned int FakeNandRowToPage(unsigned int rowAddr);
unsigned int FakeNandPhyBlockToRow(unsigned int phyBlock, unsigned int page);

/* Returns 1 if the page holds programmed data (0 = erased / never written). */
int FakeNandIsProgrammed(unsigned int ch, unsigned int way, unsigned int rowAddr);
/* Copies the full row (data + spare) into `row`; erased rows are all 0xFF. */
void FakeNandReadRow(unsigned int ch, unsigned int way, unsigned int rowAddr, unsigned char *row);
/* Returns 0 on success, non-zero if the block is bad. */
int FakeNandProgramRow(unsigned int ch, unsigned int way, unsigned int rowAddr, const unsigned char *row);
int FakeNandEraseBlock(unsigned int ch, unsigned int way, unsigned int rowAddr);

/* Bad-block list. Marking writes a non-0xFF bad-block mark at the first row's first data
 * byte, which is what FindBadBlock() looks for during a fresh-device scan. */
void FakeNandMarkBadBlock(unsigned int ch, unsigned int way, unsigned int phyBlock);
int FakeNandIsBadBlock(unsigned int ch, unsigned int way, unsigned int phyBlock);
unsigned int FakeNandBadBlockCount(void);

/* Force the next ECC read of this row to report an uncorrectable error. */
void FakeNandInjectReadFailure(unsigned int ch, unsigned int way, unsigned int rowAddr);
int FakeNandTakeReadFailure(unsigned int ch, unsigned int way, unsigned int rowAddr);

const FakeNandDieStats *FakeNandStats(unsigned int ch, unsigned int way);
FakeNandDieStats *FakeNandMutableStats(unsigned int ch, unsigned int way);
FakeNandDieStats FakeNandTotals(void);
unsigned int FakeNandProgrammedPageCount(void);

#ifdef __cplusplus
}
#endif

#endif
