#include "dare.h"

void CVirtualPlayer::Pause(int lVoiceId) {
    _tdstVirtualVoice* pVoice = reinterpret_cast<_tdstVirtualVoice*>(lVoiceId);
    pVoice->bPaused = 1;
    if (pVoice->lVoice != -1) {
        m_pstVirtFunc->pfnPause(pVoice->lVoice);
    }
}
