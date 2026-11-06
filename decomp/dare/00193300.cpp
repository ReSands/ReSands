#include "dare.h"

void CThemeManager::StopTheme(int hTheme) {
    CThemeChannel* pThemeChannel = FindThemeChannelByTheme((CTheme*)hTheme);
    if (pThemeChannel) {
        pThemeChannel->Stop((CTheme*)hTheme);
        if (pThemeChannel->CanDelete()) {
            __sndPos* pos = m_ThemeChannelList.Find(pThemeChannel, 0);
            m_ThemeChannelList.RemoveAt(pos);
            delete pThemeChannel;
        }
    }
}
