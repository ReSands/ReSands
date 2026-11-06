#include "dare.h"

void CASyncBankLoader::FreeBankWaveForms(void) {
    for (int i = 0; i < m_pBank->pBankHeader->lNbResBlock; i++) {
        if (SND_fn_bCanFreeSubBlock(m_pBank->pWaveFormData[i].lBlockID)) {
            SND_fn_vFreeSubBlock(m_pBank->pWaveFormData[i].pWaveForme, (_tdeBlockType)m_pBank->pWaveFormData[i].lBlockID);
            m_pBank->pWaveFormData[i].pWaveForme = 0;
        }
    }
    m_eLoadingState = (_tdeLoadSoundBankState)0x15;
}
