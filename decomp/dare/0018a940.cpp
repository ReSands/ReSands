#include "dare.h"

int SndManager::GetPos(int lVoice) {
    if (lVoice == 0) {
        return 0xFFFF0000;
    }
    _VOICE* pVoice = (_VOICE*)lVoice;
    if (pVoice->bToolVoice != 0) {
        CPath* pPath = pVoice->uVoice.stToolVoice.PathPtr;
        if (pPath != 0) {
            return pPath->GetPos();
        }
        return 0xFFFF0000;
    }
    return GetPos(pVoice->uVoice.stHWVoice.lHWVoice);
}
