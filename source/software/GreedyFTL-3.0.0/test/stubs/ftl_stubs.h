#ifndef FTL_STUBS_H
#define FTL_STUBS_H

#define STUB_MAX_RECORDED_REQS	512
// Virtual slice addresses interleave dies (vsa % USER_DIES == die); GC copies land on the target die.
#define STUB_GC_COPY_BLOCK		1024
#define STUB_GC_COPY_VSA(die, n)	(Vorg2VsaTranslation((die), STUB_GC_COPY_BLOCK, (n)))

typedef struct {
	unsigned int nextReqSlotTag;
	unsigned int gcCopySliceCount;
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
