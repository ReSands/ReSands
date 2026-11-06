#include "dare.h"

void CASyncBankUnLoader::UnloadEvents(void) {
    for (int i = 0; i < m_pBank->pBankHeader->lNbEvents; i++) {
        SND_fn_vStopEventBeforeUnloadSound(&m_pBank->pEventList[i]);
        m_pBank->pEventList[i].bLoaded = 0;
    }
    for (int i = 0; i < m_pBank->pBankHeader->lNbResources; i++) {
        SND_fn_vStopBeforeUnLoadResSnd(&m_pBank->pResMemList[i]);
    }
    SND_fn_vSynchroSound();
    m_eState = static_cast<_tdeUnLoadSoundBankState>(2);
}
