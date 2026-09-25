#include "fake_regs.h"

#include <stdlib.h>
#include <string.h>

#define REG_BUCKETS   1024
#define WRITE_LOG_MAX 65536

typedef struct RegNode
{
	unsigned int addr;
	unsigned int value;
	struct RegNode *next;
} RegNode;

static RegNode *buckets[REG_BUCKETS];
static FAKE_REG_WRITE writeLog[WRITE_LOG_MAX];
static unsigned int writeCount = 0;
static fake_reg_read_hook_t readHook = NULL;

static RegNode *Find(unsigned int addr, int create)
{
	unsigned int b = (addr >> 2) % REG_BUCKETS;
	RegNode *n;

	for (n = buckets[b]; n != NULL; n = n->next)
		if (n->addr == addr)
			return n;
	if (!create)
		return NULL;
	n = calloc(1, sizeof(*n));
	n->addr = addr;
	n->next = buckets[b];
	buckets[b] = n;
	return n;
}

void fake_regs_reset(void)
{
	unsigned int b;
	for (b = 0; b < REG_BUCKETS; b++)
	{
		RegNode *n = buckets[b];
		while (n != NULL)
		{
			RegNode *next = n->next;
			free(n);
			n = next;
		}
		buckets[b] = NULL;
	}
	writeCount = 0;
	readHook = NULL;
}

void fake_reg_write(unsigned int addr, unsigned int value)
{
	Find(addr, 1)->value = value;
	if (writeCount < WRITE_LOG_MAX)
	{
		writeLog[writeCount].addr = addr;
		writeLog[writeCount].value = value;
	}
	writeCount++;
}

unsigned int fake_reg_read(unsigned int addr)
{
	RegNode *n = Find(addr, 0);
	unsigned int stored = n ? n->value : 0;
	return readHook ? readHook(addr, stored) : stored;
}

void fake_reg_set(unsigned int addr, unsigned int value)
{
	Find(addr, 1)->value = value;
}

void fake_reg_set_read_hook(fake_reg_read_hook_t hook)
{
	readHook = hook;
}

unsigned int fake_reg_write_count(void)
{
	return writeCount;
}

FAKE_REG_WRITE fake_reg_write_at(unsigned int index)
{
	FAKE_REG_WRITE none = {0, 0};
	if (index >= writeCount || index >= WRITE_LOG_MAX)
		return none;
	return writeLog[index];
}

unsigned int fake_reg_writes_to(unsigned int addr, unsigned int *lastValue)
{
	unsigned int i, count = 0, limit = writeCount < WRITE_LOG_MAX ? writeCount : WRITE_LOG_MAX;
	for (i = 0; i < limit; i++)
		if (writeLog[i].addr == addr)
		{
			count++;
			if (lastValue)
				*lastValue = writeLog[i].value;
		}
	return count;
}
