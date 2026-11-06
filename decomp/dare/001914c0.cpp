#include "dare.h"

extern void FlushCache(int);
extern int sceSifSetDma(sceSifDmaData*, int);

void CAsyncTransfer::StartTransferEE_IOP(void)
{
    sceSifDmaData transData;
    transData.data = m_pSrcEEMemory;
    transData.addr = m_pIOPMemory;
    transData.size = m_ulEEIOPTransferSize;
    transData.mode = 0;

    FlushCache(0);
    m_iSifTransferQueueID = sceSifSetDma(&transData, 1);
    if (m_iSifTransferQueueID == 0) {
        m_eState = (_tdeAsyncTransfer)6;
    } else {
        m_eState = (_tdeAsyncTransfer)2;
    }
}
