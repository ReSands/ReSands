#include "dare.h"

int SndManager::TestIsPlaying(int lVoice) {
    int bRet;
    if (lVoice == 0) {
        return 0;
    }
    if (((_VOICE*)lVoice)->bToolVoice) {
        bRet = TestIsPlayingToolVoice((_VOICE*)lVoice);
    } else {
        bRet = TestIsPlaying(((_VOICE*)lVoice)->uVoice.stHWVoice.lHWVoice);
    }
    if (bRet == 0) {
        DeleteVoice((_VOICE**)&lVoice);
    }
    return bRet;
}
