#include "dare.h"

CTool* BranchPath::GetLastTool(void) const {
    if (m_pTFader) return m_pTFader;
    if (m_pTVolPan) return m_pTVolPan;
    return m_pTDecoder;
}
