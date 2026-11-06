#include "dare.h"

extern unsigned int gs_ASyncLoadResBinaryHandle;

unsigned int RPS2::ASyncLoadResBinary(_tdstBlockResourceDisk *pResDisk, _tdstBlockResourceMem *pResMem, char *pBinDataBlock, char *pExtraDataBlock) {
    unsigned int ret = this->LoadResBinary(pResDisk, pResMem, pBinDataBlock, pExtraDataBlock);
    if (ret == 0) goto fail;
    unsigned int h = gs_ASyncLoadResBinaryHandle;
    gs_ASyncLoadResBinaryHandle = h + 1;
    return h;
fail:
    return -1;
}
