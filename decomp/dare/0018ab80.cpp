#include "dare.h"

extern CRenderer* m_pRenderer__10SndManager;

void SndManager::SetMicroParam(int, _MicroParam *pParam) {
    if (m_pRenderer__10SndManager) {
        m_pRenderer__10SndManager->SetMicroParam(pParam);
    }
}
