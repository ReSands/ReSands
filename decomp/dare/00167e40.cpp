#include "dare.h"

int OneShotSAMPLEPath::GetPos(void) {
    if (m_pBranchPath) {
        return m_pBranchPath->GetPos();
    }
    return 0xFFFF0000;
}
