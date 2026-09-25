/* Model of the NVMe host controller's DMA command FIFO (HOST_TEST build only).
 *
 * host_lld.c pushes DMA descriptors by writing HOST_DMA_CMD_FIFO_REG_ADDR and polls
 * HOST_DMA_FIFO_CNT_REG_ADDR until the hardware head catches up with its tail. This
 * model observes those register writes through the fake register map, records each
 * descriptor, and advances the matching head counter so the firmware never spins. */
#ifndef FAKE_HOST_DMA_H
#define FAKE_HOST_DMA_H

#ifdef __cplusplus
extern "C" {
#endif

typedef struct
{
	unsigned int devAddr;
	unsigned int pcieAddrH;
	unsigned int pcieAddrL;
	unsigned int dmaLen;
	unsigned int autoCompletion;
	unsigned int cmd4KBOffset;
	unsigned int cmdSlotTag;
	unsigned int dmaDirection;
	unsigned int dmaType;
} FakeDmaDescriptor;

/* Resets the model and installs it as the fake register write hook. */
void FakeHostDmaReset(void);

unsigned int FakeHostDmaCount(void);
FakeDmaDescriptor FakeHostDmaAt(unsigned int index);
unsigned int FakeHostDmaCountFor(unsigned int dmaType, unsigned int dmaDirection);

#ifdef __cplusplus
}
#endif

#endif
