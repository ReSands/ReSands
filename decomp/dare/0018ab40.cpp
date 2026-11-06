#include "dare.h"

extern CRenderer* m_pRenderer__10SndManager;

int SndManager::CreateMicro(_MicroParam *pParam, int sxdId) {
    if (m_pRenderer__10SndManager) {
        m_pRenderer__10SndManager->CreateMicro(pParam);
    }
    return sxdId;
}
