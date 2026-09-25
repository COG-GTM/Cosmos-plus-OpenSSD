// Host-test stand-in for the Xilinx BSP xparameters.h.
// One NAND storage controller is mapped so USER_CHANNELS == 1 and USER_DIES == 8.
#ifndef XPARAMETERS_H
#define XPARAMETERS_H

#define XPAR_TIGER4NSC_0_BASEADDR			0x43C00000
#define XPAR_NVMEHOSTCONTROLLER_0_BASEADDR	0x83C00000
#define XPAR_SCUGIC_SINGLE_DEVICE_ID		0

#endif
