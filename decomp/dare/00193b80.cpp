#include "dare.h"

CThemeChannel::CThemeChannel(int lId) {
    m_lVoice = -1;
    m_lId = lId;
    m_pCurrentTheme = 0;
    m_pThemePlaying = 0;
    m_pNextTheme = 0;
    m_pLastTheme = 0;
    m_eSendedState = tc_eWAITING;
    m_pPartSent = 0;
    m_pPartPlaying = 0;
    m_bIndexedWillFadeOut = 0;
    m_bIndexedWillFadeIn = 0;
    m_bStartFadeAfterNextTransition = 0;
    m_rCumulPos = 0;
}
