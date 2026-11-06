#include "dare.h"

void TTransition::StartFade(int rFadeDuration, _SND_tdeFadeType eFadeType, int) {
    m_bFading = 1;
    m_lFadePos = 0;
    m_lFadeDuration = SND_fn_rMulRealRealSnd(*(int*)((char*)&m_stOutputFormat.uFormat + 4), rFadeDuration);
    m_eFadeType = eFadeType;
}
