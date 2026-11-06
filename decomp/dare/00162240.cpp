#include "dare.h"

int SMemory::SetResource(const _SND_tdstToolSourceFormat *pSourceFormat, _tdstBlockResourceMem *pBlockResourceMem) {
    if (pSourceFormat == 0) {
        return 0;
    }
    
    Reset();
    SND_fn_vMemCopySnd(&m_stSourceFormat, pSourceFormat, 0x3c);
    m_stOutputFormat.eFmt = FORMAT_BYTE;
    m_ulIndex = 0;
    m_bReturnedImminentEOS = 0;
    m_ulDeviceLatency = SND_fn_rMulRealRealSnd(0x8000, pSourceFormat->lTransferRate);
    return 1;
}
