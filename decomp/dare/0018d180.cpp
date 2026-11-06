#include "dare.h"

int CVirtualPlayer::TestIsPlaying(int lVoiceId) {
    _tdstVirtualVoice* pVoice = reinterpret_cast<_tdstVirtualVoice*>(lVoiceId);
    if (pVoice->bDead) {
        DelVirtualVoice(pVoice);
        return 0;
    }
    if (pVoice->lVoice == -1) {
        return 1;
    }
    if (!m_pstVirtFunc->pfnTestIsPlaying(pVoice->lVoice)) {
        pVoice->lVoice = -1;
        DelVirtualVoice(pVoice);
        return 0;
    }
    return 1;
}
