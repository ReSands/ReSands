#include "dare.h"

extern void *SND_fn_pvMallocSnd(unsigned int size, int zero);

__sndPos* sndCList<CThemeChannel*, CThemeChannel*>::AddHead(CThemeChannel* newElement) {
    sndCNode* oldHead = m_pNodeHead;
    sndCNode* newNode = (sndCNode*)SND_fn_pvMallocSnd(12, 0);
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
