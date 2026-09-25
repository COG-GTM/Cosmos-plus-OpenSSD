#include "mock_host_lld.h"

#include <string.h>

#include "unity.h"

#define AUTO_DMA_FIFO_DEPTH 256U

HOST_DMA_STATUS g_hostDmaStatus;
HOST_DMA_ASSIST_STATUS g_hostDmaAssistStatus;

static mock_host_call_t calls[MOCK_HOST_MAX_CALLS];
static unsigned int call_count;
static unsigned int total_calls;
static unsigned int kind_counts[MOCK_HOST_CALL_KIND_COUNT];
static unsigned int cc_en;
static unsigned int partial_done;
static mock_host_nvme_cmd_t pending[MOCK_HOST_MAX_PENDING_CMDS];
static unsigned int pending_head;
static unsigned int pending_count;
static mock_host_hook_t hook;

void mock_host_reset(void)
{
	memset(&g_hostDmaStatus, 0, sizeof(g_hostDmaStatus));
	memset(&g_hostDmaAssistStatus, 0, sizeof(g_hostDmaAssistStatus));
	memset(kind_counts, 0, sizeof(kind_counts));
	call_count = 0;
	total_calls = 0;
	cc_en = 0;
	partial_done = 1;
	pending_head = 0;
	pending_count = 0;
	hook = NULL;
}

static void record(mock_host_call_kind_t kind, unsigned int argc, const unsigned int *argv)
{
	unsigned int i;

	total_calls++;
	kind_counts[kind]++;
	if (call_count < MOCK_HOST_MAX_CALLS) {
		calls[call_count].kind = kind;
		memset(calls[call_count].args, 0, sizeof(calls[call_count].args));
		for (i = 0; i < argc; i++)
			calls[call_count].args[i] = argv[i];
		call_count++;
	}
	if (hook)
		hook(kind);
}

/* Polling calls are counted and hooked but not logged, so busy loops cannot
 * push the interesting calls out of the bounded log. */
static void poll(mock_host_call_kind_t kind)
{
	kind_counts[kind]++;
	if (hook)
		hook(kind);
}

#define RECORD(kind, ...)                                                   \
	do {                                                                    \
		const unsigned int args_[] = {0, ##__VA_ARGS__};                    \
		record(kind, sizeof(args_) / sizeof(args_[0]) - 1, args_ + 1);      \
	} while (0)

unsigned int mock_host_call_count(void) { return total_calls; }

const mock_host_call_t *mock_host_call_at(unsigned int index)
{
	return index < call_count ? &calls[index] : NULL;
}

unsigned int mock_host_count(mock_host_call_kind_t kind) { return kind_counts[kind]; }

const mock_host_call_t *mock_host_last(mock_host_call_kind_t kind)
{
	unsigned int i = call_count;

	while (i-- > 0)
		if (calls[i].kind == kind)
			return &calls[i];
	return NULL;
}

void mock_host_set_cc_en(unsigned int ccEn) { cc_en = ccEn; }
void mock_host_set_partial_done(unsigned int done) { partial_done = done; }
void mock_host_set_hook(mock_host_hook_t newHook) { hook = newHook; }
unsigned int mock_host_pending_cmds(void) { return pending_count; }

void mock_host_push_cmd(const mock_host_nvme_cmd_t *cmd)
{
	if (pending_count >= MOCK_HOST_MAX_PENDING_CMDS)
		TEST_FAIL_MESSAGE("mock_host command queue full");
	pending[(pending_head + pending_count) % MOCK_HOST_MAX_PENDING_CMDS] = *cmd;
	pending_count++;
}

void dev_irq_init() {}
void dev_irq_handler() {}

unsigned int check_nvme_cc_en()
{
	poll(MOCK_HOST_CHECK_NVME_CC_EN);
	return cc_en;
}

void set_nvme_csts_rdy(unsigned int rdy) { RECORD(MOCK_HOST_SET_NVME_CSTS_RDY, rdy); }
void set_nvme_csts_shst(unsigned int shst) { RECORD(MOCK_HOST_SET_NVME_CSTS_SHST, shst); }

void set_nvme_admin_queue(unsigned int sqValid, unsigned int cqValid, unsigned int cqIrqEn)
{
	RECORD(MOCK_HOST_SET_NVME_ADMIN_QUEUE, sqValid, cqValid, cqIrqEn);
}

unsigned int get_nvme_cmd(unsigned short *qID, unsigned short *cmdSlotTag, unsigned int *cmdSeqNum,
		unsigned int *cmdDword)
{
	const mock_host_nvme_cmd_t *cmd;

	poll(MOCK_HOST_GET_NVME_CMD);
	if (pending_count == 0)
		return 0;
	cmd = &pending[pending_head];
	*qID = cmd->qID;
	*cmdSlotTag = cmd->cmdSlotTag;
	*cmdSeqNum = cmd->cmdSeqNum;
	memcpy(cmdDword, cmd->cmdDword, sizeof(cmd->cmdDword));
	pending_head = (pending_head + 1) % MOCK_HOST_MAX_PENDING_CMDS;
	pending_count--;
	return 1;
}

void set_auto_nvme_cpl(unsigned int cmdSlotTag, unsigned int specific, unsigned int statusFieldWord)
{
	RECORD(MOCK_HOST_SET_AUTO_NVME_CPL, cmdSlotTag, specific, statusFieldWord);
}

void set_nvme_slot_release(unsigned int cmdSlotTag) { RECORD(MOCK_HOST_SET_NVME_SLOT_RELEASE, cmdSlotTag); }

void set_nvme_cpl(unsigned int sqId, unsigned int cid, unsigned int specific, unsigned int statusFieldWord)
{
	RECORD(MOCK_HOST_SET_NVME_CPL, sqId, cid, specific, statusFieldWord);
}

void set_io_sq(unsigned int ioSqIdx, unsigned int valid, unsigned int cqVector, unsigned int qSzie,
		unsigned int pcieBaseAddrL, unsigned int pcieBaseAddrH)
{
	RECORD(MOCK_HOST_SET_IO_SQ, ioSqIdx, valid, cqVector, qSzie, pcieBaseAddrL, pcieBaseAddrH);
}

void set_io_cq(unsigned int ioCqIdx, unsigned int valid, unsigned int irqEn, unsigned int irqVector,
		unsigned int qSzie, unsigned int pcieBaseAddrL, unsigned int pcieBaseAddrH)
{
	RECORD(MOCK_HOST_SET_IO_CQ, ioCqIdx, valid, irqEn, irqVector, qSzie, pcieBaseAddrL, pcieBaseAddrH);
}

void set_direct_tx_dma(unsigned int devAddr, unsigned int pcieAddrH, unsigned int pcieAddrL, unsigned int len)
{
	RECORD(MOCK_HOST_SET_DIRECT_TX_DMA, devAddr, pcieAddrH, pcieAddrL, len);
	g_hostDmaStatus.directDmaTxCnt++;
}

void set_direct_rx_dma(unsigned int devAddr, unsigned int pcieAddrH, unsigned int pcieAddrL, unsigned int len)
{
	RECORD(MOCK_HOST_SET_DIRECT_RX_DMA, devAddr, pcieAddrH, pcieAddrL, len);
	g_hostDmaStatus.directDmaRxCnt++;
}

static void advance_auto_tail(unsigned int isTx)
{
	if (isTx) {
		g_hostDmaStatus.fifoTail.autoDmaTx++;
		if (g_hostDmaStatus.fifoTail.autoDmaTx % AUTO_DMA_FIFO_DEPTH == 0)
			g_hostDmaAssistStatus.autoDmaTxOverFlowCnt++;
		g_hostDmaStatus.autoDmaTxCnt++;
	} else {
		g_hostDmaStatus.fifoTail.autoDmaRx++;
		if (g_hostDmaStatus.fifoTail.autoDmaRx % AUTO_DMA_FIFO_DEPTH == 0)
			g_hostDmaAssistStatus.autoDmaRxOverFlowCnt++;
		g_hostDmaStatus.autoDmaRxCnt++;
	}
}

void set_auto_tx_dma(unsigned int cmdSlotTag, unsigned int cmd4KBOffset, unsigned int devAddr,
		unsigned int autoCompletion)
{
	RECORD(MOCK_HOST_SET_AUTO_TX_DMA, cmdSlotTag, cmd4KBOffset, devAddr, autoCompletion);
	advance_auto_tail(1);
}

void set_auto_rx_dma(unsigned int cmdSlotTag, unsigned int cmd4KBOffset, unsigned int devAddr,
		unsigned int autoCompletion)
{
	RECORD(MOCK_HOST_SET_AUTO_RX_DMA, cmdSlotTag, cmd4KBOffset, devAddr, autoCompletion);
	advance_auto_tail(0);
}

void check_direct_tx_dma_done() { RECORD(MOCK_HOST_CHECK_DIRECT_TX_DMA_DONE); }
void check_direct_rx_dma_done() { RECORD(MOCK_HOST_CHECK_DIRECT_RX_DMA_DONE); }
void check_auto_tx_dma_done() { RECORD(MOCK_HOST_CHECK_AUTO_TX_DMA_DONE); }
void check_auto_rx_dma_done() { RECORD(MOCK_HOST_CHECK_AUTO_RX_DMA_DONE); }

unsigned int check_auto_tx_dma_partial_done(unsigned int tailIndex, unsigned int tailAssistIndex)
{
	(void)tailIndex;
	(void)tailAssistIndex;
	return partial_done;
}

unsigned int check_auto_rx_dma_partial_done(unsigned int tailIndex, unsigned int tailAssistIndex)
{
	(void)tailIndex;
	(void)tailAssistIndex;
	return partial_done;
}
