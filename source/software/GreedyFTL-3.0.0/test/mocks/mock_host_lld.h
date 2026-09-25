/*
 * Mock of the NVMe host controller low-level driver (nvme/host_lld.c).
 *
 * Records NVMe completions, queue configuration and host DMA requests so
 * tests can assert on them. DMA transfers complete immediately; the host DMA
 * FIFO tail/overflow counters in g_hostDmaStatus / g_hostDmaAssistStatus are
 * advanced like the real driver so request_transform bookkeeping stays
 * consistent. Incoming NVMe commands are served from a queue.
 */
#ifndef MOCK_HOST_LLD_H
#define MOCK_HOST_LLD_H

#include "nvme/host_lld.h"

#define MOCK_HOST_MAX_CALLS 4096
#define MOCK_HOST_MAX_PENDING_CMDS 64

typedef enum {
	MOCK_HOST_SET_AUTO_NVME_CPL,
	MOCK_HOST_SET_NVME_SLOT_RELEASE,
	MOCK_HOST_SET_NVME_CPL,
	MOCK_HOST_SET_NVME_CSTS_RDY,
	MOCK_HOST_SET_NVME_CSTS_SHST,
	MOCK_HOST_SET_NVME_ADMIN_QUEUE,
	MOCK_HOST_SET_IO_SQ,
	MOCK_HOST_SET_IO_CQ,
	MOCK_HOST_SET_DIRECT_TX_DMA,
	MOCK_HOST_SET_DIRECT_RX_DMA,
	MOCK_HOST_SET_AUTO_TX_DMA,
	MOCK_HOST_SET_AUTO_RX_DMA,
	MOCK_HOST_CHECK_DIRECT_TX_DMA_DONE,
	MOCK_HOST_CHECK_DIRECT_RX_DMA_DONE,
	MOCK_HOST_CHECK_AUTO_TX_DMA_DONE,
	MOCK_HOST_CHECK_AUTO_RX_DMA_DONE,
	MOCK_HOST_CHECK_AUTO_TX_DMA_PARTIAL_DONE,
	MOCK_HOST_CHECK_AUTO_RX_DMA_PARTIAL_DONE,
	MOCK_HOST_CHECK_NVME_CC_EN,
	MOCK_HOST_GET_NVME_CMD,
	MOCK_HOST_CALL_KIND_COUNT
} mock_host_call_kind_t;

typedef struct {
	mock_host_call_kind_t kind;
	unsigned int args[7];
} mock_host_call_t;

typedef struct {
	unsigned short qID;
	unsigned short cmdSlotTag;
	unsigned int cmdSeqNum;
	unsigned int cmdDword[16];
} mock_host_nvme_cmd_t;

/* Called at the top of every mocked function; may call fw_loop_exit(). */
typedef void (*mock_host_hook_t)(mock_host_call_kind_t kind);

void mock_host_reset(void);

unsigned int mock_host_call_count(void);
const mock_host_call_t *mock_host_call_at(unsigned int index);
unsigned int mock_host_count(mock_host_call_kind_t kind);
/* Most recent call of the given kind, or NULL. */
const mock_host_call_t *mock_host_last(mock_host_call_kind_t kind);

void mock_host_set_cc_en(unsigned int ccEn);
void mock_host_push_cmd(const mock_host_nvme_cmd_t *cmd);
unsigned int mock_host_pending_cmds(void);
/* Return values of check_auto_{tx,rx}_dma_partial_done (default 1 = done). */
void mock_host_set_partial_done(unsigned int done);
void mock_host_set_hook(mock_host_hook_t hook);

#endif /* MOCK_HOST_LLD_H */
