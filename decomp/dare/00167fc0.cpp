#include "dare.h"

void OneShotSAMPLEPath::Resume(void) const {
    if (m_pSource) {
        m_pSource->Resume();
    }
    if (m_pBranchPath) {
        m_pBranchPath->Resume();
    }
}
