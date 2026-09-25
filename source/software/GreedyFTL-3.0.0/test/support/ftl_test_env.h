/* Host test environment for GreedyFTL-3.0.0.
 *
 * The firmware addresses all of its tables through fixed physical DRAM
 * addresses (see memory_map.h). ftl_test_env_init() maps that exact address
 * range into the test process so the unmodified firmware can run on Linux.
 * It also installs the default hardware behaviour of the IO / NSC mocks. */
#ifndef FTL_TEST_ENV_H
#define FTL_TEST_ENV_H

#include <setjmp.h>
#include <stdint.h>
#include <stddef.h>
#include "mock_io.h"
#include "mock_nsc.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Emulated DRAM window (matches DRAM_START_ADDR..DRAM_END_ADDR in memory_map.h). */
#define FTL_TEST_DRAM_BASE   0x00100000u
#define FTL_TEST_DRAM_END    0x40000000u

/* Map the DRAM window (idempotent). Aborts with a message if the range is unavailable. */
void ftl_test_env_init(void);

/* Zero the DRAM window, clear the IO/NSC mocks and reinstall default behaviour.
 * Does not touch firmware globals: call the firmware Init*() functions (or
 * ftl_test_env_init_ftl()) afterwards. */
void ftl_test_env_reset(void);

/* ftl_test_env_reset() followed by the firmware's InitFTL(), i.e. the full
 * boot-time initialisation against the ideal mocked NAND array. */
void ftl_test_env_init_ftl(void);

/* Same, but with console bytes queued for the boot: "X" makes
 * InitBlockDieMap() erase the array and rebuild the bad block table. */
void ftl_test_env_init_ftl_with_console(const char *console_input);

/* Route firmware xil_printf output to stdout (default: silent unless FTL_TEST_VERBOSE=1). */
void ftl_test_set_verbose(int verbose);
extern unsigned long ftl_test_printf_count;

/* Script the UART console: inbyte() returns the queued bytes in order, then
 * '\n' forever. ftl_test_env_reset() clears the queue. InitBlockDieMap() reads
 * one byte and rebuilds the bad block table when it is 'X'. */
void ftl_test_queue_inbyte(const char *bytes);

/* Assertion capture.
 *
 *   FTL_TEST_EXPECT_ASSERT(SomeFirmwareCall());
 *
 * fails the Unity test if SomeFirmwareCall() returns without hitting a
 * firmware assert()/ASSERT(). Outside of the macro an assertion aborts the
 * test binary with the firmware's message. */
extern jmp_buf ftl_test_assert_jmp;
extern volatile int ftl_test_assert_armed;
extern volatile int ftl_test_assert_hit;
extern const char *ftl_test_last_assert_expr;

#define FTL_TEST_EXPECT_ASSERT(stmt)                                              \
	do {                                                                          \
		ftl_test_assert_hit = 0;                                                  \
		ftl_test_assert_armed = 1;                                                \
		if (setjmp(ftl_test_assert_jmp) == 0) { stmt; }                           \
		ftl_test_assert_armed = 0;                                                \
		TEST_ASSERT_TRUE_MESSAGE(ftl_test_assert_hit, "expected a firmware assert()"); \
	} while (0)

/* Default read handler installed on HOST_DMA_FIFO_CNT_REG_ADDR: reports the
 * hardware FIFO head equal to the software tail so every DMA completes
 * immediately. Tests wanting to model in-flight DMA replace it. */
uint32_t ftl_test_dma_fifo_instant_done(uintptr_t addr, uint32_t stored, void *ctx);

#ifdef __cplusplus
}
#endif
#endif
