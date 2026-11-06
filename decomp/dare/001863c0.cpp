#include "dare.h"

void TFader::GetFadeInfos(SND_tdstFadeInfo_ *pstFadInfo) {
    pstFadInfo->bFading = m_bFading;
    pstFadInfo->lFadeReminder = m_lFadeReminder;
    pstFadInfo->rCurrentFadeVolume = m_rCurrentFadeVolume_dB;
    pstFadInfo->rFadeGoalVolume_dB = m_rFadeGoalVolume_dB;
}
