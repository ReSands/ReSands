#include "dare.h"

void CVirtualPlayer::Stop(int lVoiceId) {
    _tdstVirtualVoice* pVoice = reinterpret_cast<_tdstVirtualVoice*>(lVoiceId);
    if (pVoice->lVoice != -1) {
        SwitchToVirtual(pVoice);
    }
    DelVirtualVoice(pVoice);
    SynchroVirtualVoices();
}
