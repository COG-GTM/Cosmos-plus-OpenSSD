/* Host-side stub of the Vivado-generated xparameters.h.
 *
 * The firmware derives USER_CHANNELS from the number of XPAR_TIGER4NSC_n_BASEADDR
 * macros defined here. Two channels keep every table small enough to run on a
 * developer machine / CI runner while still exercising multi-channel logic.
 * Register base addresses are outside the emulated DRAM window; all access to
 * them goes through the IO_READ32/IO_WRITE32 mock (see test/mocks/mock_io.h). */
#ifndef XPARAMETERS_H
#define XPARAMETERS_H

#define XPAR_TIGER4NSC_0_BASEADDR       0x43C00000
#define XPAR_TIGER4NSC_1_BASEADDR       0x43C10000

#define XPAR_NVMEHOSTCONTROLLER_0_BASEADDR 0x83C00000

#define XPAR_SCUGIC_SINGLE_DEVICE_ID    0
#define XPAR_CPU_CORTEXA9_0_CPU_CLK_FREQ_HZ 666666687

#endif
