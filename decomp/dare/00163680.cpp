#include "dare.h"

void BranchPath::GetFadeInfos(SND_tdstFadeInfo_ *pstFadInfo) {
    if (m_pTFader) {
        m_pTFader->GetFadeInfos(pstFadInfo);
    } else {
        pstFadInfo->bFading = 0;
        pstFadInfo->lFadeReminder = 0;
        pstFadInfo->rFadeGoalVolume_dB = 0;
        pstFadInfo->rCurrentFadeVolume = 0;
    }
}
