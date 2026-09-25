#include "mock_nsc.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include "ftl_config.h"

#define MOCK_NSC_LOG_INITIAL 8192
#define MOCK_NSC_MAX_DEVS 8
#define MOCK_NSC_MAX_WAYS 8
#define MOCK_NSC_PAGE_DATA_BYTES   BYTES_PER_DATA_REGION_OF_PAGE
#define MOCK_NSC_PAGE_SPARE_BYTES  BYTES_PER_SPARE_REGION_OF_PAGE
#define MOCK_NSC_PAGES_PER_BLOCK   PAGES_PER_MLC_BLOCK
#define MOCK_NSC_ERASED_BYTE       0xFF

typedef struct
{
	V2FMCRegisters *dev;
	unsigned int busy;
	unsigned int readyBusy;
	unsigned int statusReport[MOCK_NSC_MAX_WAYS];
	unsigned int completion[MOCK_NSC_MAX_WAYS];
	unsigned int errInfo0[MOCK_NSC_MAX_WAYS];
	unsigned int errInfo1[MOCK_NSC_MAX_WAYS];
	const void *readSrc[MOCK_NSC_MAX_WAYS];
	size_t readSrcLen[MOCK_NSC_MAX_WAYS];
	unsigned int lastTriggerRow[MOCK_NSC_MAX_WAYS];
	int used;
} dev_state_t;

static dev_state_t devs[MOCK_NSC_MAX_DEVS];

/* Contents of every programmed page, keyed by (dev, way, row). Pages that
 * were never programmed (or were erased) read back as erased NAND (0xFF). */
typedef struct page_t
{
	V2FMCRegisters *dev;
	int way;
	unsigned int row;
	unsigned char data[MOCK_NSC_PAGE_DATA_BYTES];
	unsigned char spare[MOCK_NSC_PAGE_SPARE_BYTES];
	struct page_t *next;
} page_t;

#define MOCK_NSC_PAGE_BUCKETS 4096
static page_t *pages[MOCK_NSC_PAGE_BUCKETS];

static size_t page_bucket(V2FMCRegisters *dev, int way, unsigned int row)
{
	uintptr_t h = (uintptr_t)dev ^ ((uintptr_t)way * 0x9E3779B9u) ^ ((uintptr_t)row * 0x85EBCA6Bu);
	return (h >> 4) % MOCK_NSC_PAGE_BUCKETS;
}

static page_t *page_find(V2FMCRegisters *dev, int way, unsigned int row)
{
	page_t *p = pages[page_bucket(dev, way, row)];
	for (; p; p = p->next)
		if (p->dev == dev && p->way == way && p->row == row)
			return p;
	return NULL;
}

static page_t *page_create(V2FMCRegisters *dev, int way, unsigned int row)
{
	page_t *p = page_find(dev, way, row);
	size_t b;
	if (p)
		return p;
	p = calloc(1, sizeof(*p));
	if (!p)
	{
		fprintf(stderr, "mock_nsc: out of memory storing page\n");
		abort();
	}
	p->dev = dev;
	p->way = way;
	p->row = row;
	b = page_bucket(dev, way, row);
	p->next = pages[b];
	pages[b] = p;
	return p;
}

static void page_drop(V2FMCRegisters *dev, int way, unsigned int row)
{
	page_t **pp = &pages[page_bucket(dev, way, row)];
	while (*pp)
	{
		page_t *p = *pp;
		if (p->dev == dev && p->way == way && p->row == row)
		{
			*pp = p->next;
			free(p);
			return;
		}
		pp = &p->next;
	}
}

static void pages_free_all(void)
{
	size_t b;
	for (b = 0; b < MOCK_NSC_PAGE_BUCKETS; b++)
	{
		while (pages[b])
		{
			page_t *p = pages[b];
			pages[b] = p->next;
			free(p);
		}
	}
}
static mock_nsc_call_t *log_;
static size_t log_count;
static size_t log_cap;
static size_t op_count[MOCK_NSC_OP_IS_BUSY + 1];

static void init_dev(dev_state_t *d, V2FMCRegisters *dev)
{
	int w;
	memset(d, 0, sizeof(*d));
	d->used = 1;
	d->dev = dev;
	d->readyBusy = 0xffffffffu;
	for (w = 0; w < MOCK_NSC_MAX_WAYS; w++)
	{
		d->statusReport[w] = MOCK_NSC_STATUS_PASS;
		d->completion[w] = 1;
		d->errInfo0[w] = MOCK_NSC_ERRINFO0_CLEAN;
		d->errInfo1[w] = MOCK_NSC_ERRINFO1_CLEAN;
	}
}

static dev_state_t *state(V2FMCRegisters *dev)
{
	size_t i;
	for (i = 0; i < MOCK_NSC_MAX_DEVS; i++)
		if (devs[i].used && devs[i].dev == dev)
			return &devs[i];
	for (i = 0; i < MOCK_NSC_MAX_DEVS; i++)
		if (!devs[i].used)
		{
			init_dev(&devs[i], dev);
			return &devs[i];
		}
	return &devs[0];
}

static void record(mock_nsc_op_t op, V2FMCRegisters *dev, int way, unsigned int row, void *data, void *spare)
{
	if (log_count == log_cap)
	{
		size_t cap = log_cap ? log_cap * 2 : MOCK_NSC_LOG_INITIAL;
		mock_nsc_call_t *grown = realloc(log_, cap * sizeof(*grown));
		if (!grown)
		{
			fprintf(stderr, "mock_nsc: out of memory growing call log\n");
			abort();
		}
		log_ = grown;
		log_cap = cap;
	}
	log_[log_count].op = op;
	log_[log_count].dev = dev;
	log_[log_count].way = way;
	log_[log_count].rowAddress = row;
	log_[log_count].dataBuf = data;
	log_[log_count].spareBuf = spare;
	log_count++;
	op_count[op]++;
}

void mock_nsc_reset(void)
{
	memset(devs, 0, sizeof(devs));
	log_count = 0;
	memset(op_count, 0, sizeof(op_count));
	pages_free_all();
}

const void *mock_nsc_page_data(V2FMCRegisters *dev, int way, unsigned int row)
{
	page_t *p = page_find(dev, way, row);
	return p ? p->data : NULL;
}

const void *mock_nsc_page_spare(V2FMCRegisters *dev, int way, unsigned int row)
{
	page_t *p = page_find(dev, way, row);
	return p ? p->spare : NULL;
}

void mock_nsc_set_controller_busy(V2FMCRegisters *dev, unsigned int busy) { state(dev)->busy = busy; }
void mock_nsc_set_ready_busy(V2FMCRegisters *dev, unsigned int mask) { state(dev)->readyBusy = mask; }
void mock_nsc_set_status_report(V2FMCRegisters *dev, int way, unsigned int sr) { state(dev)->statusReport[way & 7] = sr; }
void mock_nsc_set_transfer_result(V2FMCRegisters *dev, int way, unsigned int completion, unsigned int e0, unsigned int e1)
{
	dev_state_t *d = state(dev);
	d->completion[way & 7] = completion;
	d->errInfo0[way & 7] = e0;
	d->errInfo1[way & 7] = e1;
}
void mock_nsc_set_read_page_source(V2FMCRegisters *dev, int way, const void *page, size_t len)
{
	dev_state_t *d = state(dev);
	d->readSrc[way & 7] = page;
	d->readSrcLen[way & 7] = len;
}

size_t mock_nsc_call_count(void) { return log_count; }
const mock_nsc_call_t *mock_nsc_call_at(size_t i) { return i < log_count ? &log_[i] : NULL; }
size_t mock_nsc_count_op(mock_nsc_op_t op)
{
	return (size_t)op <= MOCK_NSC_OP_IS_BUSY ? op_count[op] : 0;
}

/* ---- nsc_driver.h implementation ---- */

unsigned int V2FIsControllerBusy(V2FMCRegisters *dev)
{
	record(MOCK_NSC_OP_IS_BUSY, dev, -1, 0, NULL, NULL);
	return state(dev)->busy;
}

void V2FResetSync(V2FMCRegisters *dev, int way)
{
	record(MOCK_NSC_OP_RESET, dev, way, 0, NULL, NULL);
}

void V2FSetFeaturesSync(V2FMCRegisters *dev, int way, unsigned int f02, unsigned int f10, unsigned int f01, unsigned int payLoadAddr)
{
	(void)f02; (void)f10; (void)f01;
	record(MOCK_NSC_OP_SET_FEATURES, dev, way, 0, (void *)(uintptr_t)payLoadAddr, NULL);
}

void V2FGetFeaturesSync(V2FMCRegisters *dev, int way, unsigned int *f01, unsigned int *f02, unsigned int *f10, unsigned int *f30)
{
	record(MOCK_NSC_OP_GET_FEATURES, dev, way, 0, NULL, NULL);
	if (f01) *f01 = 0;
	if (f02) *f02 = 0;
	if (f10) *f10 = 0;
	if (f30) *f30 = 0;
}

void V2FReadPageTriggerAsync(V2FMCRegisters *dev, int way, unsigned int rowAddress)
{
	record(MOCK_NSC_OP_READ_TRIGGER, dev, way, rowAddress, NULL, NULL);
	state(dev)->lastTriggerRow[way & 7] = rowAddress;
}

static size_t min_size(size_t a, size_t b) { return a < b ? a : b; }

void V2FReadPageTransferAsync(V2FMCRegisters *dev, int way, void *pageDataBuffer, void *spareDataBuffer,
                              unsigned int *errorInformation, unsigned int *completion, unsigned int rowAddress)
{
	dev_state_t *d = state(dev);
	page_t *p = page_find(dev, way, rowAddress);
	record(MOCK_NSC_OP_READ_TRANSFER, dev, way, rowAddress, pageDataBuffer, spareDataBuffer);
	if (d->readSrc[way & 7])
	{
		if (pageDataBuffer)
			memcpy(pageDataBuffer, d->readSrc[way & 7], min_size(d->readSrcLen[way & 7], MOCK_NSC_PAGE_DATA_BYTES));
	}
	else if (p)
	{
		if (pageDataBuffer)
			memcpy(pageDataBuffer, p->data, sizeof(p->data));
		if (spareDataBuffer)
			memcpy(spareDataBuffer, p->spare, sizeof(p->spare));
	}
	else
	{
		if (pageDataBuffer)
			memset(pageDataBuffer, MOCK_NSC_ERASED_BYTE, MOCK_NSC_PAGE_DATA_BYTES);
		if (spareDataBuffer)
			memset(spareDataBuffer, MOCK_NSC_ERASED_BYTE, MOCK_NSC_PAGE_SPARE_BYTES);
	}
	if (errorInformation)
	{
		errorInformation[0] = d->errInfo0[way & 7];
		errorInformation[1] = d->errInfo1[way & 7];
	}
	if (completion)
		*completion = d->completion[way & 7];
}

/* Raw transfers return one whole NAND row: data region, then spare region. */
void V2FReadPageTransferRawAsync(V2FMCRegisters *dev, int way, void *pageDataBuffer, unsigned int *completion)
{
	dev_state_t *d = state(dev);
	unsigned int row = d->lastTriggerRow[way & 7];
	page_t *p = page_find(dev, way, row);
	unsigned char *raw = pageDataBuffer;
	record(MOCK_NSC_OP_READ_TRANSFER_RAW, dev, way, row, pageDataBuffer, NULL);
	if (raw)
	{
		if (d->readSrc[way & 7])
			memcpy(raw, d->readSrc[way & 7], min_size(d->readSrcLen[way & 7], BYTES_PER_NAND_ROW));
		else
		{
			memset(raw, MOCK_NSC_ERASED_BYTE, BYTES_PER_NAND_ROW);
			if (p)
			{
				memcpy(raw, p->data, sizeof(p->data));
				memcpy(raw + BYTES_PER_DATA_REGION_OF_NAND_ROW, p->spare, sizeof(p->spare));
			}
		}
	}
	if (completion)
		*completion = d->completion[way & 7];
}

void V2FProgramPageAsync(V2FMCRegisters *dev, int way, unsigned int rowAddress, void *pageDataBuffer, void *spareDataBuffer)
{
	page_t *p;
	record(MOCK_NSC_OP_PROGRAM, dev, way, rowAddress, pageDataBuffer, spareDataBuffer);
	p = page_create(dev, way, rowAddress);
	if (pageDataBuffer)
		memcpy(p->data, pageDataBuffer, sizeof(p->data));
	if (spareDataBuffer)
		memcpy(p->spare, spareDataBuffer, sizeof(p->spare));
}

void V2FEraseBlockAsync(V2FMCRegisters *dev, int way, unsigned int rowAddress)
{
	unsigned int page;
	record(MOCK_NSC_OP_ERASE, dev, way, rowAddress, NULL, NULL);
	for (page = 0; page < MOCK_NSC_PAGES_PER_BLOCK; page++)
		page_drop(dev, way, rowAddress + page);
}

void V2FStatusCheckAsync(V2FMCRegisters *dev, int way, unsigned int *statusReport)
{
	record(MOCK_NSC_OP_STATUS_CHECK, dev, way, 0, NULL, NULL);
	if (statusReport)
		*statusReport = state(dev)->statusReport[way & 7];
}

unsigned int V2FStatusCheckSync(V2FMCRegisters *dev, int way)
{
	record(MOCK_NSC_OP_STATUS_CHECK_SYNC, dev, way, 0, NULL, NULL);
	return state(dev)->statusReport[way & 7] >> 1;
}

unsigned int V2FReadyBusyAsync(V2FMCRegisters *dev)
{
	record(MOCK_NSC_OP_READY_BUSY, dev, -1, 0, NULL, NULL);
	return state(dev)->readyBusy;
}
