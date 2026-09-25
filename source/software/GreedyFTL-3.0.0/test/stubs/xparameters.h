/* Host stand-in for the Vivado-generated xparameters.h (HOST_TEST build only).
 *
 * The firmware derives USER_CHANNELS from how many XPAR_TIGER4NSC_n_BASEADDR
 * symbols exist. HOST_TEST_CHANNELS (CMake option, default 2) selects how many
 * channels the host model exposes; fewer channels keeps the map tables small
 * enough for fast unit tests while still exercising multi-channel translation. */
#ifndef XPARAMETERS_H
#define XPARAMETERS_H

#ifndef HOST_TEST_CHANNELS
#define HOST_TEST_CHANNELS 2
#endif

#if HOST_TEST_CHANNELS < 1 || HOST_TEST_CHANNELS > 8
#error "HOST_TEST_CHANNELS must be between 1 and 8"
#endif

#define XPAR_TIGER4NSC_0_BASEADDR 0x43C00000
#if HOST_TEST_CHANNELS > 1
#define XPAR_TIGER4NSC_1_BASEADDR 0x43C10000
#endif
#if HOST_TEST_CHANNELS > 2
#define XPAR_TIGER4NSC_2_BASEADDR 0x43C20000
#endif
#if HOST_TEST_CHANNELS > 3
#define XPAR_TIGER4NSC_3_BASEADDR 0x43C30000
#endif
#if HOST_TEST_CHANNELS > 4
#define XPAR_TIGER4NSC_4_BASEADDR 0x43C40000
#endif
#if HOST_TEST_CHANNELS > 5
#define XPAR_TIGER4NSC_5_BASEADDR 0x43C50000
#endif
#if HOST_TEST_CHANNELS > 6
#define XPAR_TIGER4NSC_6_BASEADDR 0x43C60000
#endif
#if HOST_TEST_CHANNELS > 7
#define XPAR_TIGER4NSC_7_BASEADDR 0x43C70000
#endif

#define XPAR_NVMEHOSTCONTROLLER_0_BASEADDR 0x83C00000

#define XPAR_SCUGIC_SINGLE_DEVICE_ID 0
#define XPAR_SCUGIC_0_CPU_BASEADDR 0xF8F00100
#define XPAR_SCUGIC_0_DIST_BASEADDR 0xF8F01000

#define XPAR_CPU_CORTEXA9_0_CPU_CLK_FREQ_HZ 666666687
#define XPAR_PS7_DDR_0_S_AXI_BASEADDR 0x00100000
#define XPAR_PS7_DDR_0_S_AXI_HIGHADDR 0x3FFFFFFF

#include "xparameters_ps.h"

#endif
