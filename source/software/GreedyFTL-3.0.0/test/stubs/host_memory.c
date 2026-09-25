#define _GNU_SOURCE
#include "host_memory.h"

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <sys/mman.h>

#include "memory_map.h"

unsigned int hostDramBase;

static void *mappedRegion;
static size_t mappedSize;

static size_t RegionSize(void)
{
	/* FTL_MANAGEMENT_END_ADDR is relative to hostDramBase, which is 0 until mapped. */
	unsigned int savedBase = hostDramBase;
	size_t size;

	hostDramBase = 0;
	size = (size_t)FTL_MANAGEMENT_END_ADDR + 1;
	hostDramBase = savedBase;
	return size;
}

static void *TryMapAt(uintptr_t hint, size_t size)
{
	void *p = mmap((void *)hint, size, PROT_READ | PROT_WRITE,
				   MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE | MAP_FIXED_NOREPLACE, -1, 0);
	if (p == MAP_FAILED)
		return NULL;
	if ((uintptr_t)p != hint)
	{
		munmap(p, size);
		return NULL;
	}
	return p;
}

void HostMemoryInit(void)
{
	/* Same base as the real board first, so addresses in test logs match the firmware's. */
	static const uintptr_t candidates[] = {0x10000000u, 0x20000000u, 0x30000000u, 0x18000000u, 0x28000000u};
	size_t i;

	if (mappedRegion)
		return;

	mappedSize = RegionSize();
	for (i = 0; i < sizeof(candidates) / sizeof(candidates[0]); i++)
	{
		mappedRegion = TryMapAt(candidates[i], mappedSize);
		if (mappedRegion)
			break;
	}

	if (!mappedRegion)
	{
		fprintf(stderr, "host_memory: could not map %zu bytes below 4 GiB for the FTL DRAM image\n", mappedSize);
		abort();
	}

	hostDramBase = (unsigned int)(uintptr_t)mappedRegion;
}

void HostMemoryClear(void)
{
	if (!mappedRegion)
		return;
	if (madvise(mappedRegion, mappedSize, MADV_DONTNEED) != 0)
		memset(mappedRegion, 0, mappedSize);
}

unsigned int HostMemorySize(void)
{
	return (unsigned int)mappedSize;
}
