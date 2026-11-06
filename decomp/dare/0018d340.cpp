#include "dare.h"

void CVirtualPlayer::Resume(int lVoiceId) {
    _tdstVirtualVoice* pVoice = reinterpret_cast<_tdstVirtualVoice*>(lVoiceId);
    pVoice->bPaused = 0;
    if (pVoice->lVoice != -1) {
        m_pstVirtFunc->pfnResume(pVoice->lVoice);
    }
}
