#include "fake_nand.h"

#include <stdlib.h>
#include <string.h>

#include "host_dram.h"
#include "ftl_config.h"
#include "nsc_driver.h"
#include "request_schedule.h"

typedef struct fake_page {
    unsigned int channel;
    unsigned int way;
    unsigned int row;
    int programmed;
    unsigned char data[BYTES_PER_DATA_REGION_OF_PAGE];
    unsigned char spare[BYTES_PER_SPARE_REGION_OF_PAGE];
    struct fake_page *next;
} fake_page;

static fake_page *pages;
static unsigned char factory_bad[USER_CHANNELS][USER_WAYS][TOTAL_BLOCKS_PER_DIE];
static int fail_next[USER_CHANNELS][USER_WAYS];
static int pending_status_fail[USER_CHANNELS][USER_WAYS];
static unsigned int current_row[USER_CHANNELS][USER_WAYS];
static fake_nand_stats stats;
extern V2FMCRegisters *chCtlReg[USER_CHANNELS];

static void fake_nand_initialize(void) __attribute__((constructor));

static void fake_nand_initialize(void)
{
    fake_nand_reset();
}

static unsigned int nand_channel(V2FMCRegisters *dev)
{
    unsigned int channel;
    for (channel = 0; channel < USER_CHANNELS; ++channel) {
        if (dev == chCtlReg[channel])
            return channel;
    }
    return 0;
}

static unsigned int row_block(unsigned int row)
{
    return ((row % LUN_1_BASE_ADDR) / PAGES_PER_MLC_BLOCK) +
           ((row / LUN_1_BASE_ADDR) * TOTAL_BLOCKS_PER_LUN);
}

static fake_page *find_page(unsigned int channel, unsigned int way, unsigned int row, int create)
{
    fake_page *page;
    for (page = pages; page != NULL; page = page->next) {
        if (page->channel == channel && page->way == way && page->row == row)
            return page;
    }
    if (!create)
        return NULL;
    page = (fake_page *)malloc(sizeof(*page));
    if (page == NULL)
        abort();
    page->channel = channel;
    page->way = way;
    page->row = row;
    page->programmed = 0;
    memset(page->data, 0xff, sizeof(page->data));
    memset(page->spare, 0xff, sizeof(page->spare));
    page->next = pages;
    pages = page;
    return page;
}

static int consume_failure(unsigned int channel, unsigned int way, fake_nand_op op)
{
    if (fail_next[channel][way] != (int)op)
        return 0;
    fail_next[channel][way] = -1;
    if (op != FAKE_NAND_READ)
        pending_status_fail[channel][way] = 1;
    return 1;
}

void fake_nand_reset(void)
{
    fake_page *page = pages;
    while (page != NULL) {
        fake_page *next = page->next;
        free(page);
        page = next;
    }
    pages = NULL;
    memset(factory_bad, 0, sizeof(factory_bad));
    memset(fail_next, 0xff, sizeof(fail_next));
    memset(pending_status_fail, 0, sizeof(pending_status_fail));
    memset(current_row, 0, sizeof(current_row));
    memset(&stats, 0, sizeof(stats));
}

void fake_nand_stats_reset(void)
{
    memset(&stats, 0, sizeof(stats));
}

const fake_nand_stats *fake_nand_get_stats(void)
{
    return &stats;
}

void fake_nand_mark_factory_bad(unsigned int ch, unsigned int way, unsigned int block)
{
    if (ch < USER_CHANNELS && way < USER_WAYS && block < TOTAL_BLOCKS_PER_DIE)
        factory_bad[ch][way][block] = 1;
}

void fake_nand_fail_next(unsigned int ch, unsigned int way, fake_nand_op op)
{
    if (ch < USER_CHANNELS && way < USER_WAYS)
        fail_next[ch][way] = (int)op;
}

unsigned char *fake_nand_page_ptr(unsigned int ch, unsigned int way, unsigned int row)
{
    fake_page *page = find_page(ch, way, row, 1);
    return page->data;
}

int fake_nand_is_programmed(unsigned int ch, unsigned int way, unsigned int row)
{
    fake_page *page = find_page(ch, way, row, 0);
    return page != NULL && page->programmed;
}

void fake_nand_mark_programmed(unsigned int ch, unsigned int way, unsigned int row)
{
    find_page(ch, way, row, 1)->programmed = 1;
}

unsigned int V2FIsControllerBusy(V2FMCRegisters *dev)
{
    (void)dev;
    return 0;
}

void V2FResetSync(V2FMCRegisters *dev, int way)
{
    (void)dev;
    (void)way;
}

void V2FSetFeaturesSync(V2FMCRegisters *dev, int way, unsigned int f02, unsigned int f10,
                        unsigned int f01, unsigned int payload)
{
    (void)dev; (void)way; (void)f02; (void)f10; (void)f01; (void)payload;
}

void V2FGetFeaturesSync(V2FMCRegisters *dev, int way, unsigned int *f01, unsigned int *f02,
                        unsigned int *f10, unsigned int *f30)
{
    (void)dev; (void)way;
    if (f01 != NULL) *f01 = 0;
    if (f02 != NULL) *f02 = 0;
    if (f10 != NULL) *f10 = 0;
    if (f30 != NULL) *f30 = 0;
}

void V2FReadPageTriggerAsync(V2FMCRegisters *dev, int way, unsigned int row)
{
    unsigned int channel = nand_channel(dev);
    current_row[channel][way] = row;
    ++stats.read_trigger;
}

void V2FReadPageTransferAsync(V2FMCRegisters *dev, int way, void *data_buffer,
                              void *spare_buffer, unsigned int *error_info,
                              unsigned int *completion, unsigned int row)
{
    unsigned int channel = nand_channel(dev);
    fake_page *page = find_page(channel, (unsigned int)way, row, 0);
    int failed = consume_failure(channel, (unsigned int)way, FAKE_NAND_READ);
    unsigned char *data = (unsigned char *)host_dram_resolve(data_buffer);
    unsigned char *spare = (unsigned char *)host_dram_resolve(spare_buffer);
    ++stats.read_transfer;
    if (!failed) {
        if (page == NULL) {
            memset(data, 0xff, BYTES_PER_DATA_REGION_OF_PAGE);
            memset(spare, 0xff, BYTES_PER_SPARE_REGION_OF_PAGE);
        } else {
            memcpy(data, page->data, BYTES_PER_DATA_REGION_OF_PAGE);
            memcpy(spare, page->spare, BYTES_PER_SPARE_REGION_OF_PAGE);
        }
    }
    if (error_info != NULL) {
        error_info[0] = failed ? 0 : 0x11000000U;
        error_info[1] = failed ? 0 : 0xffffffffU;
    }
    if (completion != NULL)
        *completion = 1;
}

void V2FReadPageTransferRawAsync(V2FMCRegisters *dev, int way, void *data_buffer,
                                 unsigned int *completion)
{
    unsigned int channel = nand_channel(dev);
    unsigned int row = current_row[channel][way];
    unsigned int block;
    fake_page *page;
    unsigned char *buffer = (unsigned char *)host_dram_resolve(data_buffer);
    int failed = consume_failure(channel, (unsigned int)way, FAKE_NAND_READ);
    ++stats.read_raw;
    if (failed) {
        if (completion != NULL)
            *completion = 1;
        return;
    }
    page = find_page(channel, (unsigned int)way, row, 0);
    memset(buffer, 0xff, BYTES_PER_NAND_ROW);
    if (page != NULL) {
        memcpy(buffer, page->data, BYTES_PER_DATA_REGION_OF_PAGE);
        memcpy(buffer + BYTES_PER_DATA_REGION_OF_PAGE, page->spare,
               BYTES_PER_SPARE_REGION_OF_PAGE);
    }
    block = row_block(row);
    if (block < TOTAL_BLOCKS_PER_DIE && factory_bad[channel][way][block]) {
        buffer[BAD_BLOCK_MARK_BYTE0] = 0;
        buffer[BAD_BLOCK_MARK_BYTE1] = 0;
    }
    if (completion != NULL)
        *completion = 1;
}

void V2FProgramPageAsync(V2FMCRegisters *dev, int way, unsigned int row,
                         void *data_buffer, void *spare_buffer)
{
    unsigned int channel = nand_channel(dev);
    int failed = consume_failure(channel, (unsigned int)way, FAKE_NAND_PROGRAM);
    ++stats.program;
    if (!failed) {
        fake_page *page = find_page(channel, (unsigned int)way, row, 1);
        memcpy(page->data, host_dram_resolve(data_buffer), BYTES_PER_DATA_REGION_OF_PAGE);
        memcpy(page->spare, host_dram_resolve(spare_buffer), BYTES_PER_SPARE_REGION_OF_PAGE);
        page->programmed = 1;
    }
}

void V2FEraseBlockAsync(V2FMCRegisters *dev, int way, unsigned int row)
{
    unsigned int channel = nand_channel(dev);
    unsigned int block = row_block(row);
    fake_page **link = &pages;
    int failed = consume_failure(channel, (unsigned int)way, FAKE_NAND_ERASE);
    ++stats.erase;
    if (failed)
        return;
    while (*link != NULL) {
        fake_page *page = *link;
        if (page->channel == channel && page->way == (unsigned int)way &&
            row_block(page->row) == block) {
            *link = page->next;
            free(page);
        } else {
            link = &page->next;
        }
    }
}

void V2FStatusCheckAsync(V2FMCRegisters *dev, int way, unsigned int *status_report)
{
    unsigned int channel = nand_channel(dev);
    int failed = pending_status_fail[channel][way];
    pending_status_fail[channel][way] = 0;
    if (status_report != NULL)
        *status_report = failed ? (((0x60U | 1U) << 1) | 1U) : ((0x60U << 1) | 1U);
}

unsigned int V2FStatusCheckSync(V2FMCRegisters *dev, int way)
{
    unsigned int status;
    V2FStatusCheckAsync(dev, way, &status);
    return status;
}

unsigned int V2FReadyBusyAsync(V2FMCRegisters *dev)
{
    (void)dev;
    return 0xffffffffU;
}
