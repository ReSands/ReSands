#include "dare.h"

void SndManager::Pause(int lVoice) {
    _VOICE* pVoice = (_VOICE*)lVoice;
    if (pVoice) {
        if (pVoice->bToolVoice) {
            CPath* pPath = pVoice->uVoice.stToolVoice.PathPtr;
            if (pPath) {
                pPath->Pause();
                pVoice->uVoice.stToolVoice.bPaused = 1;
            }
        } else {
            m_HWVirtModule.Pause(pVoice->uVoice.stHWVoice.lHWVoice);
        }
    }
}
