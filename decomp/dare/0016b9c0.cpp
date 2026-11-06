#include "dare.h"

extern unsigned int gs_ulNbTransfersQueued;
extern void FlushCache(int);
extern int sceSifSetDma(struct sceSifDmaData *, int);
extern unsigned int gs_sifDmaQueue[];

unsigned int fn_ulSendToIOP(unsigned int uiDst, unsigned int uiSrc, unsigned int uiSize) {
    sceSifDmaData transData;
    if (uiSize == 0)
        return 0;
    transData.data = uiSrc;
    transData.addr = uiDst;
    transData.size = uiSize;
    transData.mode = 0;
    FlushCache(0);
    gs_sifDmaQueue[gs_ulNbTransfersQueued] = sceSifSetDma(&transData, 1);
    if (gs_sifDmaQueue[gs_ulNbTransfersQueued] == 0)
        return 0;
    gs_ulNbTransfersQueued++;
    return uiSize;
}
