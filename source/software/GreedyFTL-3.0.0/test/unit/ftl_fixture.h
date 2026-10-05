#ifndef GREEDYFTL_TEST_FIXTURE_H
#define GREEDYFTL_TEST_FIXTURE_H

#include "fake_nand.h"

void EvictDataBufEntry(unsigned int originReqSlotTag);
void ftl_fixture_init(void);
void ftl_fixture_init_fresh_nand(void);

#ifndef RUN_KNOWN_BUGS
#define KNOWN_BUG(message) TEST_IGNORE_MESSAGE("KNOWN BUG: " message)
#else
#define KNOWN_BUG(message)
#endif

#endif
