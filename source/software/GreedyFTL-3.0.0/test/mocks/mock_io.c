#include "mock_io.h"

#include <string.h>

#include "unity.h"

typedef struct {
	unsigned int addr;
	unsigned int value;
} reg_t;

typedef struct {
	unsigned int addr;
	unsigned int value;
} queued_read_t;

static mock_io_access_t io_log[MOCK_IO_MAX_LOG];
static unsigned int io_log_len;
static unsigned int io_log_dropped;
static reg_t regs[MOCK_IO_MAX_REGS];
static unsigned int reg_count;
static queued_read_t queued[MOCK_IO_MAX_QUEUED_READS];
static unsigned int queued_count;
static mock_io_read_hook_t read_hook;

static void log_access(mock_io_dir_t dir, unsigned int addr, unsigned int value)
{
	if (io_log_len < MOCK_IO_MAX_LOG) {
		io_log[io_log_len].dir = dir;
		io_log[io_log_len].addr = addr;
		io_log[io_log_len].value = value;
		io_log_len++;
	} else {
		io_log_dropped++;
	}
}

static reg_t *find_reg(unsigned int addr)
{
	unsigned int i;

	for (i = 0; i < reg_count; i++)
		if (regs[i].addr == addr)
			return &regs[i];
	return NULL;
}

void mock_io_set_reg(unsigned int addr, unsigned int value)
{
	reg_t *reg = find_reg(addr);

	if (!reg) {
		if (reg_count >= MOCK_IO_MAX_REGS)
			TEST_FAIL_MESSAGE("mock_io register file full; raise MOCK_IO_MAX_REGS");
		reg = &regs[reg_count++];
		reg->addr = addr;
	}
	reg->value = value;
}

unsigned int mock_io_get_reg(unsigned int addr)
{
	reg_t *reg = find_reg(addr);

	return reg ? reg->value : 0;
}

void mock_io_queue_read(unsigned int addr, unsigned int value)
{
	if (queued_count >= MOCK_IO_MAX_QUEUED_READS)
		TEST_FAIL_MESSAGE("mock_io read queue full; raise MOCK_IO_MAX_QUEUED_READS");
	queued[queued_count].addr = addr;
	queued[queued_count].value = value;
	queued_count++;
}

static int pop_queued_read(unsigned int addr, unsigned int *value)
{
	unsigned int i;

	for (i = 0; i < queued_count; i++) {
		if (queued[i].addr == addr) {
			*value = queued[i].value;
			memmove(&queued[i], &queued[i + 1], (queued_count - i - 1) * sizeof(queued[0]));
			queued_count--;
			return 1;
		}
	}
	return 0;
}

unsigned int mock_io_read32(unsigned int addr)
{
	unsigned int value;

	if (!pop_queued_read(addr, &value))
		if (!(read_hook && read_hook(addr, &value)))
			value = mock_io_get_reg(addr);
	log_access(MOCK_IO_READ, addr, value);
	return value;
}

void mock_io_write32(unsigned int addr, unsigned int value)
{
	mock_io_set_reg(addr, value);
	log_access(MOCK_IO_WRITE, addr, value);
}

void mock_io_set_read_hook(mock_io_read_hook_t hook) { read_hook = hook; }

void mock_io_reset(void)
{
	io_log_len = 0;
	io_log_dropped = 0;
	reg_count = 0;
	queued_count = 0;
	read_hook = NULL;
}

unsigned int mock_io_log_count(void) { return io_log_len; }

const mock_io_access_t *mock_io_log_at(unsigned int index)
{
	return index < io_log_len ? &io_log[index] : NULL;
}

static unsigned int count_accesses(mock_io_dir_t dir, unsigned int addr)
{
	unsigned int i, count = 0;

	for (i = 0; i < io_log_len; i++)
		if (io_log[i].dir == dir && io_log[i].addr == addr)
			count++;
	return count;
}

unsigned int mock_io_write_count(unsigned int addr) { return count_accesses(MOCK_IO_WRITE, addr); }
unsigned int mock_io_read_count(unsigned int addr) { return count_accesses(MOCK_IO_READ, addr); }

const mock_io_access_t *mock_io_last_write(unsigned int addr)
{
	unsigned int i = io_log_len;

	while (i-- > 0)
		if (io_log[i].dir == MOCK_IO_WRITE && io_log[i].addr == addr)
			return &io_log[i];
	return NULL;
}
