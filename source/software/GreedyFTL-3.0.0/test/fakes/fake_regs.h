/*
 * In-memory fake of the memory-mapped NVMe host controller registers.
 *
 * All Xil_In32/Xil_Out32 and IO_READ32/IO_WRITE32 accesses land here. Registers are
 * stored sparsely (any 32-bit address is valid), every write is journaled so tests can
 * assert on the exact sequence the firmware produced, and a small DMA engine model
 * completes host DMA descriptors as soon as they are pushed into the command FIFO so
 * the firmware's busy-wait loops terminate.
 */
#ifndef FAKE_REGS_H
#define FAKE_REGS_H

typedef struct
{
	unsigned int addr;
	unsigned int value;
} FAKE_REG_WRITE;

typedef struct
{
	unsigned int devAddr;
	unsigned int pcieAddrH;
	unsigned int pcieAddrL;
	unsigned int dmaLen;
	unsigned int autoCompletion;
	unsigned int cmd4KBOffset;
	unsigned int cmdSlotTag;
	unsigned int dmaDirection;	/* HOST_DMA_TX_DIRECTION / HOST_DMA_RX_DIRECTION */
	unsigned int dmaType;		/* HOST_DMA_DIRECT_TYPE / HOST_DMA_AUTO_TYPE */
} FAKE_DMA_DESCRIPTOR;

void fake_regs_reset(void);

unsigned int fake_reg_read32(unsigned int addr);
void fake_reg_write32(unsigned int addr, unsigned int value);

/* Pre-load a register without journaling the access (models hardware-driven state). */
void fake_reg_poke(unsigned int addr, unsigned int value);

unsigned int fake_reg_write_count(void);
unsigned int fake_reg_write_count_to(unsigned int addr);
const FAKE_REG_WRITE *fake_reg_write_at(unsigned int index);
unsigned int fake_reg_last_write_to(unsigned int addr, unsigned int *value);

unsigned int fake_dma_descriptor_count(void);
const FAKE_DMA_DESCRIPTOR *fake_dma_descriptor_at(unsigned int index);

/* Number of NVMe completions written to NVME_CPL_FIFO_REG_ADDR + 8 (the last dword of a completion). */
unsigned int fake_nvme_completion_count(void);

/* Load a 16-dword NVMe command into the command SRAM and mark the command FIFO valid. */
void fake_nvme_push_command(unsigned int qID, unsigned int cmdSlotTag, unsigned int cmdSeqNum, const unsigned int cmdDword[16]);

#endif
