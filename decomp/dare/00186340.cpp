#include "dare.h"

void TFader::StartFade(int _rFadeDuration, _SND_tdeFadeType _eFadeType, int _rGoalVolume_dB) {
    if (_rFadeDuration != 0) {
        m_rFadeDuration = _rFadeDuration;
        m_eFadeType = _eFadeType;
        if (m_bFading == 0) {
            m_rCurrentFadeVolume_dB = m_rFadeGoalVolume_dB;
        }
        m_rInitialFadeVolume_dB = m_rCurrentFadeVolume_dB;
        m_lFadeReminder = SND_fn_rMulRealRealSnd(_rFadeDuration, m_stOutputFormat.uFormat.stPCM.ulFreq);
        m_rFadeGoalVolume_dB = _rGoalVolume_dB;
        m_bFading = 1;
    } else {
        m_bFading = 0;
        m_rFadeGoalVolume_dB = _rGoalVolume_dB;
    }
}
