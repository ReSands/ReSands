#include "dare.h"

void SndManager::Resume(int lVoice) {
    _VOICE* pVoice = (_VOICE*)lVoice;
    if (pVoice) {
        if (pVoice->bToolVoice) {
            CPath* pPath = pVoice->uVoice.stToolVoice.PathPtr;
            if (pPath) {
                pPath->Resume();
                pVoice->uVoice.stToolVoice.bPaused = 0;
            }
        } else {
            m_HWVirtModule.Resume(pVoice->uVoice.stHWVoice.lHWVoice);
        }
    }
}
