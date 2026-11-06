#include "dare.h"

__sndPos* sndCList<CThemePart*, CThemePart*>::FindIndex(unsigned int nIndex) const {
    if (nIndex >= m_nCount) return 0;
    sndCNode* pNode = m_pNodeHead;
    while (nIndex--) {
        pNode = pNode->pNext;
    }
    return (__sndPos*)pNode;
}
