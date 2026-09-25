/*
 * Mock of the Tiger4 NAND storage controller driver (nsc_driver.c).
 *
 * Every V2F* call is recorded. By default the simulated NAND behaves like an
 * ideal device: every operation completes immediately and successfully, all
 * ways report ready, and page reads return erased data (0xFF). Tests change
 * that per call with the setters below or with a completion hook.
 */
#ifndef MOCK_NSC_H
#define MOCK_NSC_H

#include "nsc_driver.h"

/* Size of the inspectable call log (mock_nsc_call_at). Counters and
 * mock_nsc_last_call() keep working after the log is full. */
#define MOCK_NSC_MAX_CALLS 4096
/* V2FCommand_* values are below this bound. */
#define MOCK_NSC_MAX_CMD 64
#define MOCK_NSC_CHANNEL_UNKNOWN 0xFFFFFFFFU

/* Values reported for a successful operation (see nsc_driver.h macros). */
#define MOCK_NSC_ERROR_INFO0_PASS 0x11000000U  /* CRC valid, spare chunk valid, 0 bit errors */
#define MOCK_NSC_ERROR_INFO1_PASS 0xFFFFFFFFU  /* all page chunks valid */
#define MOCK_NSC_STATUS_REPORT_PASS ((0x60U << 1) | 1U) /* report done, complete, no fail */
#define MOCK_NSC_STATUS_REPORT_FAIL ((0x61U << 1) | 1U) /* report done, complete, fail */
#define MOCK_NSC_ALL_WAYS_READY 0xFFU
#define MOCK_NSC_ERASED_BYTE 0xFFU

typedef struct {
	unsigned int cmd;          /* V2FCommand_* */
	unsigned int channel;      /* index into chCtlReg[], or MOCK_NSC_CHANNEL_UNKNOWN */
	int way;
	unsigned int rowAddress;
	void *pageDataBuffer;
	void *spareDataBuffer;
	unsigned int *errorInformation;
	unsigned int *completion;
	unsigned int *statusReport;
} mock_nsc_call_t;

/* Invoked after the default behaviour of every recorded call. */
typedef void (*mock_nsc_hook_t)(const mock_nsc_call_t *call);

void mock_nsc_reset(void);
/* Forget recorded calls and counters but keep the scripted NAND behaviour. */
void mock_nsc_clear_calls(void);

unsigned int mock_nsc_call_count(void);
const mock_nsc_call_t *mock_nsc_call_at(unsigned int index);
unsigned int mock_nsc_count_cmd(unsigned int cmd);
const mock_nsc_call_t *mock_nsc_last_call(void);

void mock_nsc_set_hook(mock_nsc_hook_t hook);
void mock_nsc_set_controller_busy(unsigned int busy);
void mock_nsc_set_ready_busy(unsigned int readyBusy);
void mock_nsc_set_status_report(unsigned int statusReport);
void mock_nsc_set_status_sync(unsigned int status);
void mock_nsc_set_completion(unsigned int completion);
void mock_nsc_set_error_info(unsigned int errorInfo0, unsigned int errorInfo1);
/* Byte used to fill page/spare buffers on read transfers. */
void mock_nsc_set_read_fill(unsigned char fill);

#endif /* MOCK_NSC_H */
