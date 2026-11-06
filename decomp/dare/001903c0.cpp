#include "dare.h"

void CASyncBankLoader::InitBankEvents(void) {
    for (int i = 0; i < m_pstBankHeader->lNbEvents; i++) {
        if (SND_fn_bInitBinEvent(&m_pBank->pEventList[i], m_pBank->pExtraData) == 0) {
            m_pBank->pEventList[i].bLoaded = 0;
        } else {
            m_pBank->pEventList[i].bLoaded = 1;
        }
    }
    m_eLoadingState = (_tdeLoadSoundBankState)0x12;
}
