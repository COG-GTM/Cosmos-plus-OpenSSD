#include "fake_regs.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FAKE_REG_SLOTS 4096u
#define FAKE_REG_LOG_CAPACITY 65536u

typedef struct
{
	unsigned int addr;
	unsigned int value;
	unsigned char used;
} FakeRegSlot;

static FakeRegSlot regSlots[FAKE_REG_SLOTS];
static FakeRegWriteRecord writeLog[FAKE_REG_LOG_CAPACITY];
static unsigned int writeLogCount;
static FakeRegWriteHook writeHook;

static unsigned int HashAddr(unsigned int addr)
{
	return (addr >> 2) * 2654435761u;
}

static FakeRegSlot *FindSlot(unsigned int addr, int create)
{
	unsigned int index = HashAddr(addr) % FAKE_REG_SLOTS;
	unsigned int probes;

	for (probes = 0; probes < FAKE_REG_SLOTS; probes++)
	{
		FakeRegSlot *slot = &regSlots[index];
		if (!slot->used)
		{
			if (!create)
				return NULL;
			slot->used = 1;
			slot->addr = addr;
			slot->value = 0;
			return slot;
		}
		if (slot->addr == addr)
			return slot;
		index = (index + 1) % FAKE_REG_SLOTS;
	}

	fprintf(stderr, "fake_regs: register map full (%u distinct addresses)\n", FAKE_REG_SLOTS);
	abort();
}

void FakeRegsReset(void)
{
	memset(regSlots, 0, sizeof(regSlots));
	writeLogCount = 0;
	writeHook = NULL;
}

void FakeRegsSetWriteHook(FakeRegWriteHook hook)
{
	writeHook = hook;
}

unsigned int FakeRegPeek(unsigned int addr)
{
	FakeRegSlot *slot = FindSlot(addr, 0);
	return slot ? slot->value : 0;
}

void FakeRegPoke(unsigned int addr, unsigned int value)
{
	FindSlot(addr, 1)->value = value;
}

unsigned int FakeRegRead32(unsigned int addr)
{
	return FakeRegPeek(addr);
}

void FakeRegWrite32(unsigned int addr, unsigned int value)
{
	FindSlot(addr, 1)->value = value;

	if (writeLogCount < FAKE_REG_LOG_CAPACITY)
	{
		writeLog[writeLogCount].addr = addr;
		writeLog[writeLogCount].value = value;
	}
	writeLogCount++;

	if (writeHook)
		writeHook(addr, value);
}

unsigned int FakeRegsWriteCount(void)
{
	return writeLogCount;
}

FakeRegWriteRecord FakeRegsWriteAt(unsigned int index)
{
	FakeRegWriteRecord none = {0, 0};
	if (index >= writeLogCount || index >= FAKE_REG_LOG_CAPACITY)
		return none;
	return writeLog[index];
}

unsigned int FakeRegsWriteCountTo(unsigned int addr)
{
	unsigned int i, n = 0, limit = writeLogCount < FAKE_REG_LOG_CAPACITY ? writeLogCount : FAKE_REG_LOG_CAPACITY;
	for (i = 0; i < limit; i++)
		if (writeLog[i].addr == addr)
			n++;
	return n;
}

unsigned int FakeRegsLastWriteTo(unsigned int addr)
{
	unsigned int i, limit = writeLogCount < FAKE_REG_LOG_CAPACITY ? writeLogCount : FAKE_REG_LOG_CAPACITY;
	for (i = limit; i > 0; i--)
		if (writeLog[i - 1].addr == addr)
			return writeLog[i - 1].value;
	return 0;
}
