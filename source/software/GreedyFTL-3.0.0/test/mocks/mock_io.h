/* Capturable mock for memory-mapped register access (IO_READ32/IO_WRITE32,
 * Xil_In32/Xil_Out32).
 *
 * Every write is appended to a log and stored in a sparse register file; reads
 * return the stored value unless a read handler has been installed for that
 * address, which lets a test emulate hardware side effects (FIFO counters,
 * status bits that flip after a write, ...). */
#ifndef MOCK_IO_H
#define MOCK_IO_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct
{
	uintptr_t addr;
	uint32_t value;
} mock_io_write_t;

typedef uint32_t (*mock_io_read_handler_t)(uintptr_t addr, uint32_t stored, void *ctx);
typedef void (*mock_io_write_handler_t)(uintptr_t addr, uint32_t value, void *ctx);

uint32_t mock_io_read32(uintptr_t addr);
void mock_io_write32(uintptr_t addr, uint32_t value);

/* Forget every stored register, handler and logged write. */
void mock_io_reset(void);

/* Pre-load a register value that the next read will observe. */
void mock_io_set_reg(uintptr_t addr, uint32_t value);
uint32_t mock_io_get_reg(uintptr_t addr);

/* Read/write hooks for a single address. Pass NULL to remove. */
void mock_io_set_read_handler(uintptr_t addr, mock_io_read_handler_t handler, void *ctx);
void mock_io_set_write_handler(uintptr_t addr, mock_io_write_handler_t handler, void *ctx);

/* Write log accessors. */
size_t mock_io_write_count(void);
const mock_io_write_t *mock_io_write_at(size_t index);
/* Number of writes that targeted `addr`. */
size_t mock_io_write_count_for(uintptr_t addr);
/* Last value written to `addr`; returns 0 and sets *found = 0 when never written. */
uint32_t mock_io_last_write(uintptr_t addr, int *found);
size_t mock_io_read_count(void);

#ifdef __cplusplus
}
#endif
#endif
