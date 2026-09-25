/*
 * Host stub for the Xilinx BSP xparameters.h.
 *
 * Only the peripheral base addresses the firmware derives its configuration from are
 * defined. Two Tiger4 NAND storage controllers are exposed, so the host build runs with
 * USER_CHANNELS == 2 (16 dies); this keeps the FTL map tables at ~150 MB instead of the
 * ~600 MB an 8-channel configuration requires while still exercising multi-channel paths.
 * The addresses are opaque keys for the fake NAND controller and fake register map;
 * nothing is dereferenced through them.
 */
#ifndef XPARAMETERS_H
#define XPARAMETERS_H

#define XPAR_TIGER4NSC_0_BASEADDR 0x43C00000
#define XPAR_TIGER4NSC_1_BASEADDR 0x43C10000

#define XPAR_NVMEHOSTCONTROLLER_0_BASEADDR 0x83C00000

#define XPAR_SCUGIC_SINGLE_DEVICE_ID 0
#define XPAR_SCUGIC_0_DIST_BASEADDR 0xF8F01000
#define XPAR_PS7_SCUGIC_0_DEVICE_ID 0

#define XPAR_CPU_CORTEXA9_0_CPU_CLK_FREQ_HZ 666666687

#endif
