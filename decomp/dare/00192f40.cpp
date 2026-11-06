#include "dare.h"

int TAdder::GetDataSample(_SND_tdstToolBufferSample *pBuffer, unsigned int *pulNbSamplesFilled, unsigned int ulParentID) {
    if (m_lNbChildren == 0) {
        *pulNbSamplesFilled = 0;
        return 1;
    }
    if (m_eState == C_STOPPED) return 2;
    if (m_eState == C_PAUSED) return 1;
    return (this->*m_pGetDataSampleProc)(pBuffer, pulNbSamplesFilled, ulParentID);
}
