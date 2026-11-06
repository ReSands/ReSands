#include "dare.h"

void CASyncBankLoader::TestBankLoaded(void) {
    if (m_pBank->bLoaded != 0) {
        m_eLoadingState = (_tdeLoadSoundBankState)0x15;
    } else {
        m_eLoadingState = (_tdeLoadSoundBankState)1;
    }
}
