#include "fake_regs.h"

#define FAKE_REG_SLOTS 256

static struct { uintptr_t addr; uint32_t val; } regs[FAKE_REG_SLOTS];
static unsigned int regCount;
static unsigned int writeCount;

void fake_regs_reset(void)
{
	regCount = 0;
	writeCount = 0;
}

uint32_t fake_reg_read32(uintptr_t addr)
{
	unsigned int i;
	for(i = 0; i < regCount; i++)
		if(regs[i].addr == addr)
			return regs[i].val;
	return 0;
}

void fake_reg_write32(uintptr_t addr, uint32_t val)
{
	unsigned int i;
	writeCount++;
	for(i = 0; i < regCount; i++)
		if(regs[i].addr == addr)
		{
			regs[i].val = val;
			return;
		}
	if(regCount < FAKE_REG_SLOTS)
	{
		regs[regCount].addr = addr;
		regs[regCount].val = val;
		regCount++;
	}
}

unsigned int fake_reg_write_count(void)
{
	return writeCount;
}
