/*
 * Host-native stub of the BSP-generated xparameters.h.
 *
 * Only the peripherals referenced by GreedyFTL are described. The number of
 * NAND storage controllers (NSC) is selected at build time with
 * GREEDYFTL_TEST_CHANNELS (1..8) so tests can run a reduced geometry.
 * Base addresses are placeholders: the NSC and NVMe host registers are never
 * dereferenced on the host because nsc_driver.c / host_lld.c are replaced by
 * mocks and IO_READ32/IO_WRITE32 are routed to mock_io.
 */
#ifndef XPARAMETERS_H
#define XPARAMETERS_H

#ifndef GREEDYFTL_TEST_CHANNELS
#define GREEDYFTL_TEST_CHANNELS 2
#endif

#if GREEDYFTL_TEST_CHANNELS < 1 || GREEDYFTL_TEST_CHANNELS > 8
#error "GREEDYFTL_TEST_CHANNELS must be between 1 and 8"
#endif

#define XPAR_NVMEHOSTCONTROLLER_0_BASEADDR 0x83C00000U

#define XPAR_TIGER4NSC_0_BASEADDR 0x43C00000U
#if GREEDYFTL_TEST_CHANNELS > 1
#define XPAR_TIGER4NSC_1_BASEADDR 0x43C10000U
#endif
#if GREEDYFTL_TEST_CHANNELS > 2
#define XPAR_TIGER4NSC_2_BASEADDR 0x43C20000U
#endif
#if GREEDYFTL_TEST_CHANNELS > 3
#define XPAR_TIGER4NSC_3_BASEADDR 0x43C30000U
#endif
#if GREEDYFTL_TEST_CHANNELS > 4
#define XPAR_TIGER4NSC_4_BASEADDR 0x43C40000U
#endif
#if GREEDYFTL_TEST_CHANNELS > 5
#define XPAR_TIGER4NSC_5_BASEADDR 0x43C50000U
#endif
#if GREEDYFTL_TEST_CHANNELS > 6
#define XPAR_TIGER4NSC_6_BASEADDR 0x43C60000U
#endif
#if GREEDYFTL_TEST_CHANNELS > 7
#define XPAR_TIGER4NSC_7_BASEADDR 0x43C70000U
#endif

#define XPAR_PS7_SCUGIC_0_DEVICE_ID 0U
#define XPAR_SCUGIC_SINGLE_DEVICE_ID 0U
#define XPAR_FABRIC_NVMEHOSTCONTROLLER_0_DEV_IRQ_INTR 61U

#endif /* XPARAMETERS_H */
