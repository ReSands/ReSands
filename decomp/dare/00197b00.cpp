#include "dare.h"

void TAdpcm::Reset() {
    m_eState = (_SND_tdeState)0;
    m_pChild = 0;
    m_bLooping = 0;
    m_ulSampleToByte = 0;
    m_bHeaderValid = 0;
    m_bSubHeaderValid = 0;
    m_ulNbChunk = 0;
    m_BuffHeader.ulNbBytes = 0;
    m_ulSampleNo = 0;
    m_bSetNewPos = 0;
    m_ulPosSample = 0;
    m_ulNextSubHeader = 0;
    m_lLastGetdata = 0;
}
