/*
 * Host backing store for the firmware's fixed physical DRAM map.
 *
 * GreedyFTL dereferences absolute Zynq DRAM addresses (memory_map.h, e.g.
 * DATA_BUFFER_BASE_ADDR = 0x10000000) and stores pointers in 32-bit
 * "unsigned int" fields. The harness therefore maps the same address range
 * [FW_DRAM_START, FW_DRAM_END) into the test process at the identical virtual
 * addresses with an anonymous, lazily committed mapping. The executable is
 * position independent, so glibc/loader never place anything that low.
 *
 * fw_memory_reset() returns the whole range to zero-filled pages cheaply
 * (madvise(MADV_DONTNEED)), giving every test a pristine DRAM image.
 */
#ifndef FW_MEMORY_H
#define FW_MEMORY_H

#define FW_DRAM_START 0x00100000UL
#define FW_DRAM_END 0x40000000UL

/* Maps the firmware DRAM window; aborts the process if that is impossible. */
void fw_memory_init(void);
void fw_memory_reset(void);

/* Translates a firmware 32-bit address into a host pointer (identity). */
static inline void *fw_ptr(unsigned int addr) { return (void *)(unsigned long)addr; }

#endif /* FW_MEMORY_H */
