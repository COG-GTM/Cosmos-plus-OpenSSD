#ifndef XPARAMETERS_H
#define XPARAMETERS_H
/*
 * Host stub for the Vivado-generated BSP parameters.
 *
 * Only two Tiger4 NAND controller channels are declared so that
 * NUMBER_OF_CONNECTED_CHANNEL == 2 (16 dies). This keeps the mapping tables
 * (~150 MB) small enough for a CI runner while still exercising the
 * multi-channel/way striping logic. Real boards expose up to eight.
 */
#define XPAR_TIGER4NSC_0_BASEADDR 0x43C00000
#define XPAR_TIGER4NSC_1_BASEADDR 0x43C10000
#define XPAR_NVMEHOSTCONTROLLER_0_BASEADDR 0x83C00000
#define XPAR_SCUGIC_SINGLE_DEVICE_ID 0
#define XPAR_CPU_CORTEXA9_0_CPU_CLK_FREQ_HZ 666666687
#define XPAR_PS7_DDR_0_S_AXI_BASEADDR 0x00100000
#define XPAR_PS7_DDR_0_S_AXI_HIGHADDR 0x3FFFFFFF
#endif
