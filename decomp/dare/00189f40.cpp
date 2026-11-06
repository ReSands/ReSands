#include "dare.h"

void SndManager::DesInit(void) {
    m_pRenderer->RemoveChild(m_lRendererChilId);
    Destroy_TAdder(&m_pFinalAdder);
    Destroy_TPitchShifter(&m_pSlowMotion);
    Destroy_RPS2(&m_pRenderer);
}
