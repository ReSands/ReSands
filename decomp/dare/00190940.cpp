#include "dare.h"

extern void SND_fn_vFreeSubBlock(void* pDataPtr, _tdeBlockType eBlockID);
extern void SND_fn_vFreeSnd(void* pv);

void CASyncBankLoader::LoadBankAborting_FreeSubBlocksData(void) {
    if (m_pBank->pWaveFormData != 0) {
        if (m_lFreedSubBlocksData < m_lWaveFormAllocated) {
            if (m_pBank->pWaveFormData[m_lFreedSubBlocksData].pWaveForme != 0) {
                SND_fn_vFreeSubBlock(m_pBank->pWaveFormData[m_lFreedSubBlocksData].pWaveForme, (_tdeBlockType)m_pBank->pWaveFormData[m_lFreedSubBlocksData].lBlockID);
                m_pBank->pWaveFormData[m_lFreedSubBlocksData].pWaveForme = 0;
            }
            m_lFreedSubBlocksData++;
        } else {
            SND_fn_vFreeSnd(m_pBank->pWaveFormData);
            m_pBank->pWaveFormData = 0;
        }
    } else {
        m_eAbortingState = (_tdeAbortLoadState)6;
    }
}
