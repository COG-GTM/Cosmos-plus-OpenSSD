/*
 * Mock register layer behind IO_READ32/IO_WRITE32 (and Xil_In32/Xil_Out32).
 *
 * Every access is appended to an access log. Writes update a sparse register
 * file; reads return the register file value unless a read hook is installed
 * or a value was queued with mock_io_queue_read().
 */
#ifndef MOCK_IO_H
#define MOCK_IO_H

#define MOCK_IO_MAX_LOG 4096
#define MOCK_IO_MAX_REGS 512
#define MOCK_IO_MAX_QUEUED_READS 256

typedef enum { MOCK_IO_READ = 0, MOCK_IO_WRITE = 1 } mock_io_dir_t;

typedef struct {
	mock_io_dir_t dir;
	unsigned int addr;
	unsigned int value;
} mock_io_access_t;

/* Returns non-zero and sets *value to override a read. */
typedef int (*mock_io_read_hook_t)(unsigned int addr, unsigned int *value);

unsigned int mock_io_read32(unsigned int addr);
void mock_io_write32(unsigned int addr, unsigned int value);

void mock_io_reset(void);
void mock_io_set_reg(unsigned int addr, unsigned int value);
unsigned int mock_io_get_reg(unsigned int addr);
void mock_io_queue_read(unsigned int addr, unsigned int value);
void mock_io_set_read_hook(mock_io_read_hook_t hook);

unsigned int mock_io_log_count(void);
const mock_io_access_t *mock_io_log_at(unsigned int index);
unsigned int mock_io_write_count(unsigned int addr);
unsigned int mock_io_read_count(unsigned int addr);
/* Returns the most recent write to addr, or NULL. */
const mock_io_access_t *mock_io_last_write(unsigned int addr);

#endif /* MOCK_IO_H */
