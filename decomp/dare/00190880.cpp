#include "dare.h"

extern void SND_fn_vUnLoadResBinarySnd(_tdstBlockResourceMem*pstBRMem);
extern void SND_fn_vFreeSnd(void *p);

void CASyncBankLoader::LoadBankAborting_UnloadResources(void) {
    if (m_pBank->pResMemList != 0) {
        if (m_lUnloadedResources < m_lInitializedResources) {
            SND_fn_vUnLoadResBinarySnd((_tdstBlockResourceMem*)((char*)m_pBank->pResMemList + m_lUnloadedResources * 0x6c));
            m_lUnloadedResources++;
        } else {
            SND_fn_vFreeSnd(m_pBank->pResMemList);
            m_pBank->pResMemList = 0;
        }
    } else {
        m_eAbortingState = (_tdeAbortLoadState)5;
    }
}
