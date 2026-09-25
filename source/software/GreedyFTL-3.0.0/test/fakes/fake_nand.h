#ifndef FAKE_NAND_H_
#define FAKE_NAND_H_

/*
 * In-memory NAND array behind the V2F* controller API (see fake_nsc_driver.c).
 *
 * Geometry follows ftl_config.h: USER_CHANNELS x USER_WAYS dies, each with
 * TOTAL_BLOCKS_PER_DIE blocks of ROWS_PER_MLC_BLOCK rows of BYTES_PER_NAND_ROW
 * bytes. Rows are allocated lazily on first program so an untouched die costs
 * nothing. Erased bytes read back as 0xFF.
 *
 * Row addresses use the firmware encoding from request_schedule.c:
 *   rowAddr = LUN_x_BASE_ADDR + blockInLun * ROWS_PER_MLC_BLOCK + row
 */

#include <stddef.h>

typedef struct
{
	unsigned int resets;
	unsigned int setFeatures;
	unsigned int readTriggers;
	unsigned int readTransfers;
	unsigned int rawReadTransfers;
	unsigned int programs;
	unsigned int erases;
	unsigned int statusChecks;
} FAKE_NAND_DIE_STATS;

void fake_nand_reset(void);

/* Row-level access for seeding and inspecting flash contents. */
unsigned char *fake_nand_row(unsigned int ch, unsigned int way, unsigned int rowAddr);
int fake_nand_row_is_programmed(unsigned int ch, unsigned int way, unsigned int rowAddr);
unsigned int fake_nand_row_addr(unsigned int phyBlock, unsigned int row);

/* Bad-block list. Marking writes the factory bad-block mark (0x00) into row 0 and the last row. */
void fake_nand_mark_bad(unsigned int ch, unsigned int way, unsigned int phyBlock);
int fake_nand_is_bad(unsigned int ch, unsigned int way, unsigned int phyBlock);
unsigned int fake_nand_bad_block_count(unsigned int ch, unsigned int way);

/*
 * Writes a bad-block table page into `bbtPhyBlock` (the block RecoverBadBlockTable
 * expects), reflecting the current bad-block list, so InitFTL skips the full scan.
 */
void fake_nand_preload_bbt(unsigned int ch, unsigned int way, unsigned int bbtPhyBlock);

/* Fault injection: the next program/erase on the die reports a status failure. */
void fake_nand_fail_next_program(unsigned int ch, unsigned int way);
void fake_nand_fail_next_erase(unsigned int ch, unsigned int way);
/* ECC result reported on subsequent transfers: worst chunk error count and uncorrectable flag. */
void fake_nand_set_ecc_result(unsigned int ch, unsigned int way, unsigned int worstChunkErrors, int uncorrectable);

FAKE_NAND_DIE_STATS fake_nand_stats(unsigned int ch, unsigned int way);
unsigned int fake_nand_block_erase_count(unsigned int ch, unsigned int way, unsigned int phyBlock);
unsigned int fake_nand_block_program_count(unsigned int ch, unsigned int way, unsigned int phyBlock);

#endif
