/* Mock of the Tiger4 NAND storage controller driver (nsc_driver.c).
 *
 * nsc_driver.c talks to the controller through a register struct at the NSC
 * AXI base address and busy-waits on it, so it is replaced wholesale by this
 * mock. Every V2F* call is recorded; the default behaviour models an "ideal"
 * NAND array: the controller is never busy, every way is ready, transfers
 * complete immediately with a clean ECC report and status checks report success.
 * Tests override that per channel/way to model busy dies, failed programs,
 * ECC warnings or uncorrectable reads. */
#ifndef MOCK_NSC_H
#define MOCK_NSC_H

#include <stddef.h>
#include "nsc_driver.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum
{
	MOCK_NSC_OP_RESET = 1,
	MOCK_NSC_OP_SET_FEATURES,
	MOCK_NSC_OP_GET_FEATURES,
	MOCK_NSC_OP_READ_TRIGGER,
	MOCK_NSC_OP_READ_TRANSFER,
	MOCK_NSC_OP_READ_TRANSFER_RAW,
	MOCK_NSC_OP_PROGRAM,
	MOCK_NSC_OP_ERASE,
	MOCK_NSC_OP_STATUS_CHECK,
	MOCK_NSC_OP_STATUS_CHECK_SYNC,
	MOCK_NSC_OP_READY_BUSY,
	MOCK_NSC_OP_IS_BUSY
} mock_nsc_op_t;

typedef struct
{
	mock_nsc_op_t op;
	V2FMCRegisters *dev;
	int way;
	unsigned int rowAddress;
	void *dataBuf;
	void *spareBuf;
} mock_nsc_call_t;

/* Status report value written by V2FStatusCheckAsync: 0xC1 = report-done | complete | pass */
#define MOCK_NSC_STATUS_PASS       0xC1u
/* complete | fail bits set */
#define MOCK_NSC_STATUS_FAIL       0xC3u
/* report-done but not complete yet -> firmware re-issues the status check */
#define MOCK_NSC_STATUS_PENDING    0x01u

/* Error info written by V2FReadPageTransferAsync: CRC valid, spare valid, 0 bit errors. */
#define MOCK_NSC_ERRINFO0_CLEAN    0x11000000u
#define MOCK_NSC_ERRINFO1_CLEAN    0xffffffffu

void mock_nsc_reset(void);

/* Behaviour knobs (apply to subsequent calls). */
void mock_nsc_set_controller_busy(V2FMCRegisters *dev, unsigned int busy);
void mock_nsc_set_ready_busy(V2FMCRegisters *dev, unsigned int readyBusyMask);
void mock_nsc_set_status_report(V2FMCRegisters *dev, int way, unsigned int statusReport);
void mock_nsc_set_transfer_result(V2FMCRegisters *dev, int way, unsigned int completion,
                                  unsigned int errorInfo0, unsigned int errorInfo1);
/* Bytes copied into the page buffer on every read transfer for (dev, way); NULL disables. */
void mock_nsc_set_read_page_source(V2FMCRegisters *dev, int way, const void *page, size_t len);

/* The mock keeps the contents of every programmed page: V2FProgramPageAsync
 * stores data+spare by (dev, way, row), V2FReadPageTransferAsync returns them
 * (unless a read-page source override is set), and V2FEraseBlockAsync drops
 * all pages of the block. Pages never programmed (or erased) read back as
 * 0xFF, like real NAND; raw reads return data followed by spare at
 * BYTES_PER_DATA_REGION_OF_NAND_ROW. Read-source overrides are clamped to the
 * destination size. These accessors return NULL for a page holding no data. */
const void *mock_nsc_page_data(V2FMCRegisters *dev, int way, unsigned int row);
const void *mock_nsc_page_spare(V2FMCRegisters *dev, int way, unsigned int row);

/* Call log. */
size_t mock_nsc_call_count(void);
const mock_nsc_call_t *mock_nsc_call_at(size_t index);
size_t mock_nsc_count_op(mock_nsc_op_t op);

#ifdef __cplusplus
}
#endif
#endif
