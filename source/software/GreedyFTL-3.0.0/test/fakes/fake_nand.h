/*
 * Test-side controls for the in-memory NAND model behind the fake nsc_driver.c.
 *
 * Geometry follows ftl_config.h: one V2FMCRegisters "device" per channel, USER_WAYS
 * ways per channel, two LUNs per way addressed through LUN_0/LUN_1_BASE_ADDR row
 * addresses. Pages are materialised lazily when programmed; unwritten or erased
 * pages read back as 0xFF. Factory bad blocks carry a bad-block mark (byte 0 of the
 * data and spare regions of the first and last row is 0x00) that survives erases,
 * exactly what the firmware's FindBadBlock() scans for.
 */
#ifndef FAKE_NAND_H
#define FAKE_NAND_H

typedef struct
{
	unsigned int readTriggers;
	unsigned int readTransfers;
	unsigned int rawReadTransfers;
	unsigned int programs;
	unsigned int erases;
	unsigned int resets;
	unsigned int setFeatures;
	unsigned int statusChecks;
} FAKE_NAND_STATS;

void fake_nand_reset(void);
const FAKE_NAND_STATS *fake_nand_stats(void);
void fake_nand_reset_stats(void);

void fake_nand_mark_factory_bad(unsigned int chNo, unsigned int wayNo, unsigned int phyBlockNo);
unsigned int fake_nand_is_factory_bad(unsigned int chNo, unsigned int wayNo, unsigned int phyBlockNo);

/* Make the next `count` program / erase operations on a die report a NAND failure. */
void fake_nand_inject_program_fail(unsigned int chNo, unsigned int wayNo, unsigned int count);
void fake_nand_inject_erase_fail(unsigned int chNo, unsigned int wayNo, unsigned int count);
/* Make the next `count` ECC-on read transfers on a die report an uncorrectable ECC error. */
void fake_nand_inject_ecc_fail(unsigned int chNo, unsigned int wayNo, unsigned int count);

/* Data region of a programmed page, or NULL when the page is in the erased state. */
const unsigned char *fake_nand_page_data(unsigned int chNo, unsigned int wayNo, unsigned int rowAddr);
const unsigned char *fake_nand_page_spare(unsigned int chNo, unsigned int wayNo, unsigned int rowAddr);
unsigned int fake_nand_written_page_count(void);

unsigned int fake_nand_row_to_phy_block(unsigned int rowAddr);
unsigned int fake_nand_row_to_page(unsigned int rowAddr);
unsigned int fake_nand_phy_block_to_row(unsigned int phyBlockNo, unsigned int pageNo);

#endif
