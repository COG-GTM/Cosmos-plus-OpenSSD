#ifndef HOST_TEST_XIL_IO_H
#define HOST_TEST_XIL_IO_H
#include <stdint.h>
uint32_t Xil_In32(uintptr_t address);
void Xil_Out32(uintptr_t address, uint32_t value);
uint16_t Xil_In16(uintptr_t address);
void Xil_Out16(uintptr_t address, uint16_t value);
uint8_t Xil_In8(uintptr_t address);
void Xil_Out8(uintptr_t address, uint8_t value);
#endif
