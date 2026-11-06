#include "dare.h"

void CVirtualPlayer::SwitchToVirtual(_tdstVirtualVoice *pVoice) {
    m_pstVirtFunc->pfnStop(pVoice->lVoice);
    pVoice->lVoice = -1;
}
