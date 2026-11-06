#include "dare.h"

CThemeChannel::_stPartInfo sndCList<CThemeChannel::_stPartInfo, CThemeChannel::_stPartInfo>::RemoveHead(void) {
    sndCNode* pOldNode = m_pNodeHead;
    CThemeChannel::_stPartInfo returnValue = pOldNode->data;
    m_pNodeHead = pOldNode->pNext;
    if (m_pNodeHead != 0) {
        m_pNodeHead->pPrev = 0;
    } else {
        m_pNodeTail = 0;
    }
    if (pOldNode != 0) {
        SND_fn_vFreeSnd(pOldNode);
        m_nCount--;
    }
    return returnValue;
}
