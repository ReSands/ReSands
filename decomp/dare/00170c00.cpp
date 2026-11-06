#include "dare.h"

int TVirtualSample::SetLoopSampleInfo(_SND_tdstLoopSampleInfo *pstLoopSampleInfo) {
    if (pstLoopSampleInfo->ulNbSamples == 0) {
        return 0;
    }
    m_lSampleLength = pstLoopSampleInfo->ulNbSamples;
    m_rSampleLengthSec = SND_fn_rDivRealRealSnd(pstLoopSampleInfo->ulNbSamples, m_ulInputFreq);
    return 1;
}
