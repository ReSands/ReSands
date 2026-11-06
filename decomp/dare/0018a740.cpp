#include "dare.h"

extern CRenderer* m_pRenderer__10SndManager;

void SndManager::Synchro(void) {
    m_HWVirtModule.Synchro();
    if (m_pRenderer__10SndManager) {
        m_pRenderer__10SndManager->Synchro();
    }
}
