#include "dare.h"

void ThemePath::StartFade(int rFadeDuration, _SND_tdeFadeType eFadeType, int rEndVol_dB, void (*fade_callback)(int), int param_callback) {
    m_bFading = 1;
    m_pfnEndOfFadeCB = fade_callback;
    m_lEndOfFadeParamCB = param_callback;
    if (m_pOneShotPath) {
        m_pOneShotPath->StartFade(rFadeDuration, eFadeType, rEndVol_dB, 0, 0);
    }
    m_pTTransition->StartFade(rFadeDuration, eFadeType, rEndVol_dB);
}
