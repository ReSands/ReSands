#include "dare.h"

void ThemePath_EndOfFadeCallback(int lParam) {
    ThemePath* pThemePath = (ThemePath*)lParam;
    if (pThemePath == 0) return;
    pThemePath->m_bFading = 0;
    pThemePath->m_bTransiting = 0;
    pThemePath->m_pTTransition->RemoveChild(pThemePath->m_lTransitionChildId);
    if (pThemePath->m_pOneShotPath != 0) {
        Destroy_OneShotPath(&pThemePath->m_pOneShotPath);
        pThemePath->m_pOneShotPath = 0;
    }
    if (pThemePath->m_pOneShotPathNext != 0) {
        Destroy_OneShotPath(&pThemePath->m_pOneShotPathNext);
        pThemePath->m_pOneShotPathNext = 0;
    }
    pThemePath->m_pResMixed = 0;
    pThemePath->m_pResNext = 0;
    pThemePath->m_pResToBeSet = 0;
    if (pThemePath->m_pfnEndOfFadeCB != 0) {
        pThemePath->m_pfnEndOfFadeCB(pThemePath->m_lEndOfFadeParamCB);
    }
}
