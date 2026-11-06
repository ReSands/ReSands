#include "dare.h"
void CThemeManager::SynchroTheme(void)
{
    sndCList<CThemeChannel *, CThemeChannel *>::sndCNode *pNode = m_ThemeChannelList.m_pNodeHead;
    while (pNode) {
        CThemeChannel *pChannel = pNode->data;
        pNode = pNode->pNext;
        pChannel->SynchroTheme();
    }
}
