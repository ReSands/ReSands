#include "dare.h"

void CASyncBankUnLoader::UnloadResources(void)
{
    if (m_lUnLoadedResources < m_pBank->pBankHeader->lNbResources) {
        SND_fn_vUnLoadResBinarySnd(&m_pBank->pResMemList[m_lUnLoadedResources]);
        m_lUnLoadedResources++;
    } else {
        if (m_pBank->pResMemList != 0) {
            SND_fn_vFreeSnd(m_pBank->pResMemList);
            m_pBank->pResMemList = 0;
        }
        if (m_pBank->pExtraData != 0) {
            SND_fn_vFreeSnd(m_pBank->pExtraData);
            m_pBank->pExtraData = 0;
        }
        m_eState = (_tdeUnLoadSoundBankState)3;
    }
}
