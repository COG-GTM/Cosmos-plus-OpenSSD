#define _GNU_SOURCE
#include "host_dram.h"

#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <unistd.h>

#include "memory_map.h"

uintptr_t host_dram_base;
static void *dram_reservation;

static void host_dram_initialize(void) __attribute__((constructor));

static void host_dram_initialize(void)
{
    const size_t gib = (size_t)1 << 30;
    const size_t reserve_size = (size_t)5 << 30;
    int flags = MAP_PRIVATE | MAP_ANON;
#ifdef MAP_NORESERVE
    flags |= MAP_NORESERVE;
#endif

    dram_reservation = mmap(NULL, reserve_size, PROT_NONE, flags, -1, 0);
    if (dram_reservation == MAP_FAILED) {
        perror("host DRAM mmap");
        abort();
    }
    host_dram_base = ((uintptr_t)dram_reservation + 0xffffffffULL) & ~0xffffffffULL;
    if (host_dram_base < (uintptr_t)dram_reservation ||
        host_dram_base + gib > (uintptr_t)dram_reservation + reserve_size ||
        mprotect((void *)host_dram_base, gib, PROT_READ | PROT_WRITE) != 0) {
        perror("host DRAM mprotect");
        abort();
    }
    if ((uintptr_t)FTL_MANAGEMENT_END_ADDR - host_dram_base >= gib) {
        fprintf(stderr, "FTL tables exceed the 1 GiB host DRAM arena\n");
        abort();
    }
}

void *host_dram_resolve(const void *address)
{
    uintptr_t value = (uintptr_t)address;
    return value < 0x100000000ULL ? (void *)(host_dram_base + value) : (void *)address;
}

uintptr_t host_dram_offset(const void *address)
{
    uintptr_t value = (uintptr_t)address;
    return value >= host_dram_base ? value - host_dram_base : value;
}
