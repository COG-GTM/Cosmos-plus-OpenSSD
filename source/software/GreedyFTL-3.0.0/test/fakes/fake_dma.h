#ifndef FAKE_DMA_H_
#define FAKE_DMA_H_

/*
 * Inspection API for the fake NVMe host controller / DMA engine implemented in
 * fake_host_lld.c. Every DMA descriptor and NVMe completion issued through the
 * host_lld.h API is logged here in issue order.
 */

typedef struct
{
	unsigned int dmaType;        /* HOST_DMA_DIRECT_TYPE / HOST_DMA_AUTO_TYPE */
	unsigned int dmaDirection;   /* HOST_DMA_TX_DIRECTION / HOST_DMA_RX_DIRECTION */
	unsigned int devAddr;
	unsigned int pcieAddrH;
	unsigned int pcieAddrL;
	unsigned int len;
	unsigned int cmdSlotTag;
	unsigned int cmd4KBOffset;
	unsigned int autoCompletion;
} FAKE_DMA_DESC;

typedef struct
{
	unsigned int cplType;        /* ONLY_CPL_TYPE / AUTO_CPL_TYPE / CMD_SLOT_RELEASE_TYPE */
	unsigned int cmdSlotTag;
	unsigned int sqId;
	unsigned int cid;
	unsigned int specific;
	unsigned int statusFieldWord;
} FAKE_NVME_CPL;

void fake_dma_reset(void);

/* When enabled (default) every DMA completes as soon as it is issued. */
void fake_dma_set_auto_complete(int enabled);
/* Marks all outstanding DMAs complete (advances the hardware FIFO head to the tail). */
void fake_dma_complete_all(void);

unsigned int fake_dma_count(void);
FAKE_DMA_DESC fake_dma_at(unsigned int index);
unsigned int fake_dma_count_of(unsigned int dmaType, unsigned int dmaDirection);

unsigned int fake_nvme_cpl_count(void);
FAKE_NVME_CPL fake_nvme_cpl_at(unsigned int index);

/* Queue an NVMe command for get_nvme_cmd() by writing the fake command FIFO/SRAM registers. */
void fake_nvme_push_cmd(unsigned int qID, unsigned int cmdSlotTag, unsigned int cmdSeqNum, const unsigned int cmdDword[16]);

#endif
