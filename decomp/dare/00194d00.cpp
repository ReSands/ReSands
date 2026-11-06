#include "dare.h"
int CThemeChannel::IsThemeUsed(CTheme *pTheme)
{
    if (pTheme == m_pCurrentTheme)
        return 1;
    if (pTheme == m_pNextTheme)
        return 1;
    return m_OldThemeList.Find(pTheme, 0) != 0;
}
