#include "dare.h"

void CThemeChannel::SetNextTheme(CTheme *pNextTheme) {
    if (m_pCurrentTheme == 0) {
        m_pCurrentTheme = pNextTheme;
    } else {
        if (m_pNextTheme != 0) {
            m_OldThemeList.AddTail(m_pNextTheme);
        }
        m_pNextTheme = pNextTheme;
        if (m_pCurrentTheme->GetThemeRes()->uRes.stTheme.eTransition == TRANSITION_ATENDOFPART_INDEXED) {
            StartFadeOut();
        }
    }
}
