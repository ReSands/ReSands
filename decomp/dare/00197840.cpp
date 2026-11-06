#include "dare.h"

extern void *SND_fn_pvMallocSnd(unsigned int size, int flags);

int TAdpcm::SetOutputFormat(const _SND_tdstToolFormat *pFormat) {
    int bRet = CTool::SetOutputFormat(pFormat);
    if (bRet) {
        if (pFormat->uFormat.stPCM.uwResolution == 0x10) {
            m_ulSampleToByte = pFormat->uFormat.stPCM.uwNbChannels * 2;
        } else {
            bRet = 0;
        }
        int tmp = pFormat->uFormat.stPCM.uwNbChannels * 13;
        m_CoderStateArray = (_state_cod *)SND_fn_pvMallocSnd(tmp << 2, 0);
    }
    return bRet;
}
