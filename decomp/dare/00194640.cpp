#include "dare.h"


int CThemeChannel::IsThemePlaying(CTheme *pTheme)
{
    if (pTheme == 0)
        return 0;
    __sndPos *pos = m_OldThemeList.Find(pTheme, 0);
    if (pos != 0 && pTheme->IsPlaying() == 0) {
        delete pTheme;
        m_OldThemeList.RemoveAt(pos);
        return 0;
    }
    if (pTheme == m_pNextTheme)
        return 1;
    return 1;
}
