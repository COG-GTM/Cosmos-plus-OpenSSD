#include "mock_nsc.h"

#include <string.h>

#include "ftl_config.h"

static mock_nsc_call_t calls[MOCK_NSC_MAX_CALLS];
static unsigned int call_count;
static unsigned int total_calls;
static unsigned int cmd_counts[MOCK_NSC_MAX_CMD];
static mock_nsc_call_t last_call;
static mock_nsc_hook_t hook;
static unsigned int controller_busy;
static unsigned int ready_busy;
static unsigned int status_report;
static unsigned int status_sync;
static unsigned int completion_value;
static unsigned int error_info0;
static unsigned int error_info1;
static unsigned char read_fill;

void mock_nsc_clear_calls(void)
{
	call_count = 0;
	total_calls = 0;
	memset(cmd_counts, 0, sizeof(cmd_counts));
	memset(&last_call, 0, sizeof(last_call));
}

void mock_nsc_reset(void)
{
	mock_nsc_clear_calls();
	hook = NULL;
	controller_busy = 0;
	ready_busy = MOCK_NSC_ALL_WAYS_READY;
	status_report = MOCK_NSC_STATUS_REPORT_PASS;
	status_sync = 0x60U;
	completion_value = 1U;
	error_info0 = MOCK_NSC_ERROR_INFO0_PASS;
	error_info1 = MOCK_NSC_ERROR_INFO1_PASS;
	read_fill = MOCK_NSC_ERASED_BYTE;
}

static unsigned int channel_of(V2FMCRegisters *dev)
{
	unsigned int ch;

	for (ch = 0; ch < USER_CHANNELS; ch++)
		if (chCtlReg[ch] == dev)
			return ch;
	return MOCK_NSC_CHANNEL_UNKNOWN;
}

static void record(mock_nsc_call_t *call)
{
	total_calls++;
	if (call->cmd < MOCK_NSC_MAX_CMD)
		cmd_counts[call->cmd]++;
	last_call = *call;
	if (call_count < MOCK_NSC_MAX_CALLS)
		calls[call_count++] = *call;
	if (hook)
		hook(call);
}

static mock_nsc_call_t make_call(unsigned int cmd, V2FMCRegisters *dev, int way, unsigned int rowAddress)
{
	mock_nsc_call_t call;

	memset(&call, 0, sizeof(call));
	call.cmd = cmd;
	call.channel = channel_of(dev);
	call.way = way;
	call.rowAddress = rowAddress;
	return call;
}

unsigned int mock_nsc_call_count(void) { return total_calls; }

const mock_nsc_call_t *mock_nsc_call_at(unsigned int index)
{
	return index < call_count ? &calls[index] : NULL;
}

unsigned int mock_nsc_count_cmd(unsigned int cmd)
{
	return cmd < MOCK_NSC_MAX_CMD ? cmd_counts[cmd] : 0;
}

const mock_nsc_call_t *mock_nsc_last_call(void)
{
	return total_calls ? &last_call : NULL;
}

void mock_nsc_set_hook(mock_nsc_hook_t newHook) { hook = newHook; }
void mock_nsc_set_controller_busy(unsigned int busy) { controller_busy = busy; }
void mock_nsc_set_ready_busy(unsigned int readyBusy) { ready_busy = readyBusy; }
void mock_nsc_set_status_report(unsigned int statusReport) { status_report = statusReport; }
void mock_nsc_set_status_sync(unsigned int status) { status_sync = status; }
void mock_nsc_set_completion(unsigned int completion) { completion_value = completion; }
void mock_nsc_set_read_fill(unsigned char fill) { read_fill = fill; }

void mock_nsc_set_error_info(unsigned int errorInfo0, unsigned int errorInfo1)
{
	error_info0 = errorInfo0;
	error_info1 = errorInfo1;
}

unsigned int V2FIsControllerBusy(V2FMCRegisters *dev)
{
	(void)dev;
	return controller_busy;
}

void V2FResetSync(V2FMCRegisters *dev, int way)
{
	mock_nsc_call_t call = make_call(V2FCommand_Reset, dev, way, 0);

	record(&call);
}

void V2FSetFeaturesSync(V2FMCRegisters *dev, int way, unsigned int feature0x02, unsigned int feature0x10,
		unsigned int feature0x01, unsigned int payLoadAddr)
{
	mock_nsc_call_t call = make_call(V2FCommand_SetFeatures, dev, way, 0);

	(void)feature0x02;
	(void)feature0x10;
	(void)feature0x01;
	(void)payLoadAddr;
	record(&call);
}

void V2FGetFeaturesSync(V2FMCRegisters *dev, int way, unsigned int *feature0x01, unsigned int *feature0x02,
		unsigned int *feature0x10, unsigned int *feature0x30)
{
	mock_nsc_call_t call = make_call(V2FCommand_GetFeatures, dev, way, 0);

	*feature0x01 = 0;
	*feature0x02 = 0;
	*feature0x10 = 0;
	*feature0x30 = 0;
	record(&call);
}

void V2FReadPageTriggerAsync(V2FMCRegisters *dev, int way, unsigned int rowAddress)
{
	mock_nsc_call_t call = make_call(V2FCommand_ReadPageTrigger, dev, way, rowAddress);

	record(&call);
}

void V2FReadPageTransferAsync(V2FMCRegisters *dev, int way, void *pageDataBuffer, void *spareDataBuffer,
		unsigned int *errorInformation, unsigned int *completion, unsigned int rowAddress)
{
	mock_nsc_call_t call = make_call(V2FCommand_ReadPageTransfer, dev, way, rowAddress);

	call.pageDataBuffer = pageDataBuffer;
	call.spareDataBuffer = spareDataBuffer;
	call.errorInformation = errorInformation;
	call.completion = completion;
	if (pageDataBuffer)
		memset(pageDataBuffer, read_fill, BYTES_PER_DATA_REGION_OF_PAGE);
	if (spareDataBuffer)
		memset(spareDataBuffer, read_fill, BYTES_PER_SPARE_REGION_OF_PAGE);
	if (errorInformation) {
		errorInformation[0] = error_info0;
		errorInformation[1] = error_info1;
	}
	if (completion)
		*completion = completion_value;
	record(&call);
}

void V2FReadPageTransferRawAsync(V2FMCRegisters *dev, int way, void *pageDataBuffer, unsigned int *completion)
{
	mock_nsc_call_t call = make_call(V2FCommand_ReadPageTransferRaw, dev, way, 0);

	call.pageDataBuffer = pageDataBuffer;
	call.completion = completion;
	if (pageDataBuffer)
		memset(pageDataBuffer, read_fill, BYTES_PER_NAND_ROW);
	if (completion)
		*completion = completion_value;
	record(&call);
}

void V2FProgramPageAsync(V2FMCRegisters *dev, int way, unsigned int rowAddress, void *pageDataBuffer,
		void *spareDataBuffer)
{
	mock_nsc_call_t call = make_call(V2FCommand_ProgramPage, dev, way, rowAddress);

	call.pageDataBuffer = pageDataBuffer;
	call.spareDataBuffer = spareDataBuffer;
	record(&call);
}

void V2FEraseBlockAsync(V2FMCRegisters *dev, int way, unsigned int rowAddress)
{
	mock_nsc_call_t call = make_call(V2FCommand_BlockErase, dev, way, rowAddress);

	record(&call);
}

void V2FStatusCheckAsync(V2FMCRegisters *dev, int way, unsigned int *statusReport)
{
	mock_nsc_call_t call = make_call(V2FCommand_StatusCheck, dev, way, 0);

	call.statusReport = statusReport;
	if (statusReport)
		*statusReport = status_report;
	record(&call);
}

unsigned int V2FStatusCheckSync(V2FMCRegisters *dev, int way)
{
	mock_nsc_call_t call = make_call(V2FCommand_StatusCheck, dev, way, 0);

	record(&call);
	return status_sync;
}

unsigned int V2FReadyBusyAsync(V2FMCRegisters *dev)
{
	(void)dev;
	return ready_busy;
}
