#include "dare.h"

extern CRenderer* m_pRenderer__10SndManager;
void SND_fn_vDesInitSourceModule__Fv(void);

void SndManager::PostDesInitEngineConf(void) {
    if (m_pRenderer__10SndManager) {
        m_pRenderer__10SndManager->PostDesInitEngineConf();
    }
    SND_fn_vDesInitSourceModule__Fv();
}
