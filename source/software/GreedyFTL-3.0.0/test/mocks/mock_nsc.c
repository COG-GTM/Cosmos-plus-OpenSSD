#include "mock_nsc.h"
#include <string.h>
#include "ftl_config.h"

#define MOCK_NSC_LOG_SIZE 8192
#define MOCK_NSC_MAX_DEVS 8
#define MOCK_NSC_MAX_WAYS 8

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
	int used;
} dev_state_t;

static dev_state_t devs[MOCK_NSC_MAX_DEVS];
static mock_nsc_call_t log_[MOCK_NSC_LOG_SIZE];
static size_t log_count;

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
	if (log_count < MOCK_NSC_LOG_SIZE)
	{
		log_[log_count].op = op;
		log_[log_count].dev = dev;
		log_[log_count].way = way;
		log_[log_count].rowAddress = row;
		log_[log_count].dataBuf = data;
		log_[log_count].spareBuf = spare;
	}
	log_count++;
}

void mock_nsc_reset(void)
{
	memset(devs, 0, sizeof(devs));
	log_count = 0;
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
const mock_nsc_call_t *mock_nsc_call_at(size_t i) { return (i < log_count && i < MOCK_NSC_LOG_SIZE) ? &log_[i] : NULL; }
size_t mock_nsc_count_op(mock_nsc_op_t op)
{
	size_t i, n = 0, limit = log_count < MOCK_NSC_LOG_SIZE ? log_count : MOCK_NSC_LOG_SIZE;
	for (i = 0; i < limit; i++)
		if (log_[i].op == op)
			n++;
	return n;
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
}

void V2FReadPageTransferAsync(V2FMCRegisters *dev, int way, void *pageDataBuffer, void *spareDataBuffer,
                              unsigned int *errorInformation, unsigned int *completion, unsigned int rowAddress)
{
	dev_state_t *d = state(dev);
	record(MOCK_NSC_OP_READ_TRANSFER, dev, way, rowAddress, pageDataBuffer, spareDataBuffer);
	if (d->readSrc[way & 7] && pageDataBuffer)
		memcpy(pageDataBuffer, d->readSrc[way & 7], d->readSrcLen[way & 7]);
	if (errorInformation)
	{
		errorInformation[0] = d->errInfo0[way & 7];
		errorInformation[1] = d->errInfo1[way & 7];
	}
	if (completion)
		*completion = d->completion[way & 7];
}

void V2FReadPageTransferRawAsync(V2FMCRegisters *dev, int way, void *pageDataBuffer, unsigned int *completion)
{
	dev_state_t *d = state(dev);
	record(MOCK_NSC_OP_READ_TRANSFER_RAW, dev, way, 0, pageDataBuffer, NULL);
	if (d->readSrc[way & 7] && pageDataBuffer)
		memcpy(pageDataBuffer, d->readSrc[way & 7], d->readSrcLen[way & 7]);
	if (completion)
		*completion = d->completion[way & 7];
}

void V2FProgramPageAsync(V2FMCRegisters *dev, int way, unsigned int rowAddress, void *pageDataBuffer, void *spareDataBuffer)
{
	record(MOCK_NSC_OP_PROGRAM, dev, way, rowAddress, pageDataBuffer, spareDataBuffer);
}

void V2FEraseBlockAsync(V2FMCRegisters *dev, int way, unsigned int rowAddress)
{
	record(MOCK_NSC_OP_ERASE, dev, way, rowAddress, NULL, NULL);
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
