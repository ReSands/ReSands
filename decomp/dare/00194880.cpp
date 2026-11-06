#include "dare.h"


void CThemeChannel::Stop(CTheme *pTheme)
{
    if (m_lVoice != 0 && m_lVoice != -1) {
        SND_fn_vStopTransitionSnd(m_lVoice);
        m_lVoice = -1;
    }
    m_pThemePlaying = 0;
    if (m_pCurrentTheme != 0) {
        m_pCurrentTheme->Stop();
        m_OldThemeList.AddTail(m_pCurrentTheme);
        m_pCurrentTheme = 0;
    }
    if (m_pNextTheme != 0) {
        m_pNextTheme->Stop();
        m_OldThemeList.AddTail(m_pNextTheme);
        m_pNextTheme = 0;
    }
    __sndPos* pos = m_OldThemeList.Find(pTheme, 0);
    if (pos != 0) {
        pTheme->Stop();
        delete pTheme;
        m_OldThemeList.RemoveAt(pos);
    }
    m_PartSentList.RemoveAll();
    m_eSendedState = tc_eDONE;
}
