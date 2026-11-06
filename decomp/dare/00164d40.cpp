#include "dare.h"

void LoopingPath::Pause(void) const {
    if (m_pTTransition) {
        m_pTTransition->Pause();
    }
    if (m_pOneShotPath) {
        m_pOneShotPath->Pause();
    }
}
