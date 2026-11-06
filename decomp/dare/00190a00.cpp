#include "dare.h"

extern void SND_fn_vFreeSnd(void* pv);

void CASyncBankLoader::LoadBankAborting_FreeMemoryAndResetFields(void) {
    if (m_stBlockTransfer.stDoubleBuffer[0].pvReadBuffer != 0) {
        SND_fn_vFreeSnd(m_stBlockTransfer.stDoubleBuffer[0].pvReadBuffer);
        m_stBlockTransfer.stDoubleBuffer[0].pvReadBuffer = 0;
    }
    if (m_stBlockTransfer.stDoubleBuffer[1].pvReadBuffer != 0) {
        SND_fn_vFreeSnd(m_stBlockTransfer.stDoubleBuffer[1].pvReadBuffer);
        m_stBlockTransfer.stDoubleBuffer[1].pvReadBuffer = 0;
    }
    if (m_pResDisk != 0) {
        SND_fn_vFreeSnd(m_pResDisk);
        m_pResDisk = 0;
    }
    if (m_pstBankHeader != 0) {
        SND_fn_vFreeSnd(m_pstBankHeader);
        m_pstBankHeader = 0;
        m_pBank->pBankHeader = 0;
    }
    if (m_pBank != 0) {
        if (m_pBank->pExtraData != 0) {
            SND_fn_vFreeSnd(m_pBank->pExtraData);
            m_pBank->pExtraData = 0;
        }
        m_pBank = 0;
    }
    m_lWaveFormAllocated = 0;
    m_lInitializedResources = 0;
    m_lInitializedEvents = 0;
    m_eAbortingState = (_tdeAbortLoadState)7;
    m_eLoadingState = (_tdeLoadSoundBankState)0x14;
}
