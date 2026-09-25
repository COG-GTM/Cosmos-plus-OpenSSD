#ifndef FAKE_REGS_H_
#define FAKE_REGS_H_

/*
 * In-memory register map backing Xil_In32/Xil_Out32 and IO_READ32/IO_WRITE32.
 * Every write is appended to a log so tests can assert on exact register
 * traffic; reads return the last written value (or a test-installed hook).
 */

typedef struct
{
	unsigned int addr;
	unsigned int value;
} FAKE_REG_WRITE;

typedef unsigned int (*fake_reg_read_hook_t)(unsigned int addr, unsigned int storedValue);

void fake_regs_reset(void);
void fake_reg_write(unsigned int addr, unsigned int value);
unsigned int fake_reg_read(unsigned int addr);

/* Preload a register value without recording a write. */
void fake_reg_set(unsigned int addr, unsigned int value);
void fake_reg_set_read_hook(fake_reg_read_hook_t hook);

unsigned int fake_reg_write_count(void);
FAKE_REG_WRITE fake_reg_write_at(unsigned int index);
/* Number of logged writes to `addr`; last value written is stored in *lastValue if non-NULL. */
unsigned int fake_reg_writes_to(unsigned int addr, unsigned int *lastValue);

#endif
