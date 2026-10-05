#ifndef HOST_TEST_FAKE_NAND_H
#define HOST_TEST_FAKE_NAND_H
#include <stddef.h>
#include <stdint.h>
typedef enum {
    FAKE_NAND_READ = 0,
    FAKE_NAND_PROGRAM = 1,
    FAKE_NAND_ERASE = 2
} fake_nand_op;
typedef struct {
    unsigned long read_trigger;
    unsigned long read_transfer;
    unsigned long read_raw;
    unsigned long program;
    unsigned long erase;
} fake_nand_stats;
void fake_nand_reset(void);
void fake_nand_stats_reset(void);
const fake_nand_stats *fake_nand_get_stats(void);
void fake_nand_mark_factory_bad(unsigned int ch, unsigned int way, unsigned int block);
/* Fails the next op on (ch, way) via its status check; FAKE_NAND_READ fails the read trigger. */
void fake_nand_fail_next(unsigned int ch, unsigned int way, fake_nand_op op);
unsigned char *fake_nand_page_ptr(unsigned int ch, unsigned int way, unsigned int row);
int fake_nand_is_programmed(unsigned int ch, unsigned int way, unsigned int row);
void fake_nand_mark_programmed(unsigned int ch, unsigned int way, unsigned int row);
#endif
