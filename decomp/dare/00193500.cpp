#include "dare.h"

CThemeChannel* CThemeManager::FindThemeChannelByTheme(CTheme *pTheme) {
    CThemeChannel* channel;
    sndCList<CThemeChannel *, CThemeChannel *>::sndCNode* node = this->m_ThemeChannelList.m_pNodeHead;
    while (node) {
        channel = node->data;
        node = node->pNext;
        if (channel->IsThemeUsed(pTheme)) {
            return channel;
        }
    }
    return 0;
}
