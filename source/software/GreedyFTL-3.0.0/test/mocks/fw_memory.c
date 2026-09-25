#define _GNU_SOURCE
#include "fw_memory.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

#ifndef MAP_FIXED_NOREPLACE
#define MAP_FIXED_NOREPLACE 0x100000
#endif

static int mapped;

void fw_memory_init(void)
{
	void *base = (void *)FW_DRAM_START;
	size_t len = FW_DRAM_END - FW_DRAM_START;
	void *got;

	if (mapped)
		return;

	got = mmap(base, len, PROT_READ | PROT_WRITE,
			MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE | MAP_FIXED_NOREPLACE, -1, 0);
	if (got != base) {
		fprintf(stderr, "fw_memory: cannot map firmware DRAM window [0x%lx, 0x%lx): %s\n",
				FW_DRAM_START, FW_DRAM_END, got == MAP_FAILED ? strerror(errno) : "address taken");
		abort();
	}
	mapped = 1;
}

void fw_memory_reset(void)
{
	fw_memory_init();
	if (madvise((void *)FW_DRAM_START, FW_DRAM_END - FW_DRAM_START, MADV_DONTNEED) != 0) {
		perror("fw_memory: madvise");
		abort();
	}
}
