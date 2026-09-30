#define _GNU_SOURCE
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

#include "host_mem.h"
#include "memory_map.h"

unsigned int hostDramBaseAddr;
unsigned int hostAdminCmdDataBufferAddr;

static void *arena;
static size_t arenaSize;

static size_t ftlRegionSize(void)
{
	unsigned int savedBase = hostDramBaseAddr;
	size_t size;

	hostDramBaseAddr = 0;
	size = (size_t)FTL_MANAGEMENT_END_ADDR + 1;
	hostDramBaseAddr = savedBase;
	return size;
}

static void *mapBelow4GiB(size_t size)
{
	int flags = MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE;
	void *p = MAP_FAILED;

#ifdef MAP_32BIT
	p = mmap(NULL, size, PROT_READ | PROT_WRITE, flags | MAP_32BIT, -1, 0);
#endif
	if (p == MAP_FAILED)
		p = mmap((void *)(uintptr_t)0x10000000, size, PROT_READ | PROT_WRITE, flags, -1, 0);
	if (p == MAP_FAILED)
		return NULL;
	if ((uintptr_t)p + size > 0xFFFFFFFFull)
	{
		munmap(p, size);
		return NULL;
	}
	return p;
}

void host_mem_init(void)
{
	if (arena)
		return;

	arenaSize = ftlRegionSize() + HOST_ADMIN_CMD_DATA_BUFFER_SIZE;
	arenaSize = (arenaSize + 0xFFFF) & ~(size_t)0xFFFF;

	arena = mapBelow4GiB(arenaSize);
	if (!arena)
	{
		fprintf(stderr, "host_mem_init: cannot map %zu bytes below 4 GiB\n", arenaSize);
		abort();
	}

	hostDramBaseAddr = (unsigned int)(uintptr_t)arena;
	hostAdminCmdDataBufferAddr = (unsigned int)(uintptr_t)arena + (unsigned int)(arenaSize - HOST_ADMIN_CMD_DATA_BUFFER_SIZE);
}

void host_mem_clear(void)
{
	host_mem_init();
	/* Dropping the pages rather than memset keeps untouched map regions lazily allocated. */
	if (madvise(arena, arenaSize, MADV_DONTNEED) != 0)
		memset(arena, 0, arenaSize);
}

size_t host_mem_size(void)
{
	host_mem_init();
	return arenaSize;
}
