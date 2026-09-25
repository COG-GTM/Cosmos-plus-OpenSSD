#ifndef FTL_STUBS_H
#define FTL_STUBS_H

#define STUB_MAX_RECORDED_REQS	512
#define STUB_FREE_VSA_BASE		0x100000

typedef struct {
	unsigned int nextReqSlotTag;
	unsigned int nextFreeVsa;
	unsigned int allocateTempDataBufCalls;
	unsigned int updateTempDataBufCalls;
	unsigned int findFreeVsaForGcCalls;
	unsigned int lastGcCopyDie;
	unsigned int lastGcVictimBlock;
	unsigned int selectLowLevelReqQCalls;
	unsigned int issuedReqSlotTags[STUB_MAX_RECORDED_REQS];
	unsigned int eraseBlockCalls;
	unsigned int lastEraseDie;
	unsigned int lastEraseBlock;
} STUB_STATE;

extern STUB_STATE stub;

void stub_reset(void);

#endif
