#include "dare.h"

extern void *SND_fn_pvMallocSnd(unsigned int size, int flags);

__sndPos* sndCList<CThemeChannel::_stPartInfo, CThemeChannel::_stPartInfo>::AddHead(CThemeChannel::_stPartInfo newElement) {
    sndCNode* oldHead = m_pNodeHead;
    sndCNode* newNode = (sndCNode*)SND_fn_pvMallocSnd(0x10, 0);
    if (newNode) {
        newNode->pNext = oldHead;
        newNode->pPrev = 0;
        m_nCount++;
    }
    newNode->data = newElement;
    if (m_pNodeHead != 0) {
        m_pNodeHead->pPrev = newNode;
    } else {
        m_pNodeTail = newNode;
    }
    m_pNodeHead = newNode;
    return (__sndPos*)newNode;
}
