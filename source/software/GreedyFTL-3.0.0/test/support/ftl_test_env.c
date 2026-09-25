#define _GNU_SOURCE
#include "ftl_test_env.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <errno.h>
#include "memory_map.h"
#include "nvme/host_lld.h"

#ifndef MAP_FIXED_NOREPLACE
#define MAP_FIXED_NOREPLACE 0x100000
#endif

static void *dram;
static const size_t dram_len = FTL_TEST_DRAM_END - FTL_TEST_DRAM_BASE;

unsigned long ftl_test_printf_count;
jmp_buf ftl_test_assert_jmp;
volatile int ftl_test_assert_armed;
volatile int ftl_test_assert_hit;
const char *ftl_test_last_assert_expr;
volatile int ftl_test_escape_hit;

void ftl_test_assert_failed(const char *expr, const char *file, int line)
{
	ftl_test_last_assert_expr = expr;
	if (ftl_test_assert_armed)
	{
		ftl_test_assert_hit = 1;
		longjmp(ftl_test_assert_jmp, 1);
	}
	fprintf(stderr, "\nfirmware assertion failed: %s (%s:%d)\n", expr, file, line);
	abort();
}

void ftl_test_env_escape(void)
{
	if (ftl_test_assert_armed)
	{
		ftl_test_escape_hit = 1;
		longjmp(ftl_test_assert_jmp, 1);
	}
	fprintf(stderr, "\nftl_test_env_escape() called outside FTL_TEST_RUN_UNTIL_ESCAPE\n");
	abort();
}

uint32_t ftl_test_dma_fifo_instant_done(uintptr_t addr, uint32_t stored, void *ctx)
{
	(void)addr; (void)stored; (void)ctx;
	return g_hostDmaStatus.fifoTail.dword;
}

void ftl_test_env_init(void)
{
	void *want = (void *)(uintptr_t)FTL_TEST_DRAM_BASE;
	if (dram)
		return;
	dram = mmap(want, dram_len, PROT_READ | PROT_WRITE,
	            MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE | MAP_FIXED_NOREPLACE, -1, 0);
	if (dram == MAP_FAILED || dram != want)
	{
		fprintf(stderr, "ftl_test_env: cannot map emulated DRAM at %p (%zu bytes): %s\n"
		        "  The test binary must be position independent (-pie) so the low 1 GiB is free.\n",
		        want, dram_len, dram == MAP_FAILED ? strerror(errno) : "address in use");
		if (dram != MAP_FAILED)
			munmap(dram, dram_len);
		abort();
	}
}

void ftl_test_env_reset(void)
{
	ftl_test_env_init();
	/* Drop all pages: they read back as zero and are re-faulted lazily. */
	if (madvise(dram, dram_len, MADV_DONTNEED) != 0)
		memset(dram, 0, dram_len);
	mock_io_reset();
	mock_nsc_reset();
	memset(&g_hostDmaStatus, 0, sizeof(g_hostDmaStatus));
	memset(&g_hostDmaAssistStatus, 0, sizeof(g_hostDmaAssistStatus));
	mock_io_set_read_handler(HOST_DMA_FIFO_CNT_REG_ADDR, ftl_test_dma_fifo_instant_done, NULL);
	ftl_test_queue_inbyte(NULL);
	ftl_test_set_printf_hook(NULL, NULL);
	ftl_test_printf_count = 0;
	ftl_test_escape_hit = 0;
	ftl_test_assert_armed = 0;
	ftl_test_assert_hit = 0;
}

void ftl_test_env_init_ftl_with_console(const char *console_input)
{
	ftl_test_env_reset();
	ftl_test_queue_inbyte(console_input);
	InitFTL();
}

void ftl_test_env_init_ftl(void)
{
	ftl_test_env_init_ftl_with_console(NULL);
}
