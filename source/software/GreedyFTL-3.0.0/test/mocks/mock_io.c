#include "mock_io.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

#define MOCK_IO_MAX_REGS      1024
#define MOCK_IO_MAX_HANDLERS  64
#define MOCK_IO_LOG_INITIAL   4096

typedef struct { uintptr_t addr; uint32_t value; int used; } reg_t;
typedef struct { uintptr_t addr; mock_io_read_handler_t rd; mock_io_write_handler_t wr; void *rd_ctx; void *wr_ctx; int used; } handler_t;

static reg_t regs[MOCK_IO_MAX_REGS];
static handler_t handlers[MOCK_IO_MAX_HANDLERS];
static mock_io_write_t *log_;
static size_t log_count;
static size_t log_cap;
static size_t read_count;

static reg_t *find_reg(uintptr_t addr, int create)
{
	size_t i;
	for (i = 0; i < MOCK_IO_MAX_REGS; i++)
		if (regs[i].used && regs[i].addr == addr)
			return &regs[i];
	if (!create)
		return NULL;
	for (i = 0; i < MOCK_IO_MAX_REGS; i++)
		if (!regs[i].used)
		{
			regs[i].used = 1;
			regs[i].addr = addr;
			regs[i].value = 0;
			return &regs[i];
		}
	return NULL;
}

static handler_t *find_handler(uintptr_t addr, int create)
{
	size_t i;
	for (i = 0; i < MOCK_IO_MAX_HANDLERS; i++)
		if (handlers[i].used && handlers[i].addr == addr)
			return &handlers[i];
	if (!create)
		return NULL;
	for (i = 0; i < MOCK_IO_MAX_HANDLERS; i++)
		if (!handlers[i].used)
		{
			memset(&handlers[i], 0, sizeof(handlers[i]));
			handlers[i].used = 1;
			handlers[i].addr = addr;
			return &handlers[i];
		}
	return NULL;
}

uint32_t mock_io_read32(uintptr_t addr)
{
	reg_t *r = find_reg(addr, 0);
	handler_t *h = find_handler(addr, 0);
	uint32_t stored = r ? r->value : 0;
	read_count++;
	if (h && h->rd)
		return h->rd(addr, stored, h->rd_ctx);
	return stored;
}

void mock_io_write32(uintptr_t addr, uint32_t value)
{
	reg_t *r = find_reg(addr, 1);
	handler_t *h = find_handler(addr, 0);
	if (r)
		r->value = value;
	if (log_count == log_cap)
	{
		size_t cap = log_cap ? log_cap * 2 : MOCK_IO_LOG_INITIAL;
		mock_io_write_t *grown = realloc(log_, cap * sizeof(*grown));
		if (!grown)
		{
			fprintf(stderr, "mock_io: out of memory growing write log\n");
			abort();
		}
		log_ = grown;
		log_cap = cap;
	}
	log_[log_count].addr = addr;
	log_[log_count].value = value;
	log_count++;
	if (h && h->wr)
		h->wr(addr, value, h->wr_ctx);
}

void mock_io_reset(void)
{
	memset(regs, 0, sizeof(regs));
	memset(handlers, 0, sizeof(handlers));
	log_count = 0;
	read_count = 0;
}

void mock_io_set_reg(uintptr_t addr, uint32_t value)
{
	reg_t *r = find_reg(addr, 1);
	if (r)
		r->value = value;
}

uint32_t mock_io_get_reg(uintptr_t addr)
{
	reg_t *r = find_reg(addr, 0);
	return r ? r->value : 0;
}

void mock_io_set_read_handler(uintptr_t addr, mock_io_read_handler_t handler, void *ctx)
{
	handler_t *h = find_handler(addr, 1);
	if (h)
	{
		h->rd = handler;
		h->rd_ctx = ctx;
	}
}

void mock_io_set_write_handler(uintptr_t addr, mock_io_write_handler_t handler, void *ctx)
{
	handler_t *h = find_handler(addr, 1);
	if (h)
	{
		h->wr = handler;
		h->wr_ctx = ctx;
	}
}

size_t mock_io_write_count(void)
{
	return log_count;
}

const mock_io_write_t *mock_io_write_at(size_t index)
{
	if (index >= log_count)
		return NULL;
	return &log_[index];
}

size_t mock_io_write_count_for(uintptr_t addr)
{
	size_t i, n = 0;
	for (i = 0; i < log_count; i++)
		if (log_[i].addr == addr)
			n++;
	return n;
}

uint32_t mock_io_last_write(uintptr_t addr, int *found)
{
	size_t i;
	for (i = log_count; i > 0; i--)
		if (log_[i - 1].addr == addr)
		{
			if (found) *found = 1;
			return log_[i - 1].value;
		}
	if (found) *found = 0;
	return 0;
}

size_t mock_io_read_count(void)
{
	return read_count;
}
