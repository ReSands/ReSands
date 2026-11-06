#include "dare.h"

void CASyncBankUnLoader::FreeSubBlocksData(void)
{
    if (m_lWaveFormFreed < m_pBank->pBankHeader->lNbResBlock) {
        if (!SND_fn_bCanFreeSubBlock(m_pBank->pWaveFormData[m_lWaveFormFreed].lBlockID)) {
            SND_fn_vFreeSubBlock(m_pBank->pWaveFormData[m_lWaveFormFreed].pWaveForme, 
                                 static_cast<_tdeBlockType>(m_pBank->pWaveFormData[m_lWaveFormFreed].lBlockID));
        }
        m_lWaveFormFreed++;
    } else {
        if (m_pBank->pWaveFormData != 0) {
            SND_fn_vFreeSnd(m_pBank->pWaveFormData);
            m_pBank->pWaveFormData = 0;
        }
        m_eState = static_cast<_tdeUnLoadSoundBankState>(4);
    }
}
