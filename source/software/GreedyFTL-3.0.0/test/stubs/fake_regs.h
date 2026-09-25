/* In-memory fake of the memory-mapped register space (HOST_TEST build only).
 *
 * Xil_In32/Xil_Out32 and IO_READ32/IO_WRITE32 resolve to FakeRegRead32/FakeRegWrite32.
 * Every write is appended to a log so tests can assert on register traffic, and a
 * single write hook lets a hardware model (fake_host_dma.c) react to writes. */
#ifndef FAKE_REGS_H
#define FAKE_REGS_H

#ifdef __cplusplus
extern "C" {
#endif

typedef struct
{
	unsigned int addr;
	unsigned int value;
} FakeRegWriteRecord;

typedef void (*FakeRegWriteHook)(unsigned int addr, unsigned int value);

/* Firmware-facing accessors. */
unsigned int FakeRegRead32(unsigned int addr);
void FakeRegWrite32(unsigned int addr, unsigned int value);

/* Test-facing helpers. */
void FakeRegsReset(void);
unsigned int FakeRegPeek(unsigned int addr);
void FakeRegPoke(unsigned int addr, unsigned int value);
void FakeRegsSetWriteHook(FakeRegWriteHook hook);

unsigned int FakeRegsWriteCount(void);
FakeRegWriteRecord FakeRegsWriteAt(unsigned int index);
unsigned int FakeRegsWriteCountTo(unsigned int addr);
unsigned int FakeRegsLastWriteTo(unsigned int addr);

#ifdef __cplusplus
}
#endif

#endif
