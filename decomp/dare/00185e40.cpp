#include "dare.h"

TFader::TFader(void)
{
    m_pChild = 0;
    m_bFading = 0;
    m_lFadeReminder = 0;
    m_rFadeGoalVolume_dB = 0;
    m_rCurrentFadeVolume_dB = 0;
    m_rFadeGoalVolume_dB = 0;
    m_rParentFadeVol_dB = 0;
}
