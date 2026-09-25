#include "host_memory.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

#include "memory_map.h"

unsigned int hostTestDramBase = 0;

static void *arena = NULL;
static size_t arenaSize = 0;

static size_t FtlManagementSize(void)
{
	/* Base cancels out, so this is valid even before the arena exists. */
	return (size_t)(FTL_MANAGEMENT_END_ADDR + 1 - DATA_BUFFER_BASE_ADDR);
}

static void *MapBelow4G(size_t size)
{
	int flags = MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE;
	void *p;

	/* Prefer the firmware's own DRAM address so host and target layouts match. */
	p = mmap((void *)0x10000000, size, PROT_READ | PROT_WRITE, flags, -1, 0);
	if (p != MAP_FAILED && (uintptr_t)p + size <= 0xFFFFFFFFu)
		return p;
	if (p != MAP_FAILED)
		munmap(p, size);

#ifdef MAP_32BIT
	p = mmap(NULL, size, PROT_READ | PROT_WRITE, flags | MAP_32BIT, -1, 0);
	if (p != MAP_FAILED && (uintptr_t)p + size <= 0xFFFFFFFFu)
		return p;
	if (p != MAP_FAILED)
		munmap(p, size);
#endif
	return NULL;
}

void host_memory_init(void)
{
	if (arena == NULL)
	{
		arenaSize = HOST_TEST_ADMIN_CMD_BUFFER_SIZE + FtlManagementSize();
		arena = MapBelow4G(arenaSize);
		if (arena == NULL)
		{
			fprintf(stderr, "host_memory: cannot map %zu bytes below 4 GiB\n", arenaSize);
			abort();
		}
		hostTestDramBase = (unsigned int)((uintptr_t)arena + HOST_TEST_ADMIN_CMD_BUFFER_SIZE);
	}
	else
	{
		/* Re-zero for test isolation; MADV_DONTNEED drops the pages cheaply. */
		if (madvise(arena, arenaSize, MADV_DONTNEED) != 0)
			memset(arena, 0, arenaSize);
	}
}

size_t host_memory_size(void)
{
	return arenaSize;
}

void *host_memory_ptr(unsigned int addr)
{
	return (void *)(uintptr_t)addr;
}
