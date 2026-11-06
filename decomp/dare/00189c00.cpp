#include "dare.h"

extern _SND_tdeAudioMode m_tdeAudioModeDARE;
extern _SND_tdeAudioMode m_stdeAudioModeHW;

int SndManager::SetAudioModeTool(_SND_tdeAudioMode eAudioMode) {
    switch (eAudioMode) {
        case SND_E_STEREO:
            m_tdeAudioModeDARE = eAudioMode;
            m_stdeAudioModeHW = (_SND_tdeAudioMode)0;
            return 1;
        case SND_E_DOLBYPL2:
            m_tdeAudioModeDARE = eAudioMode;
            m_stdeAudioModeHW = (_SND_tdeAudioMode)2;
            return 1;
        default:
            m_stdeAudioModeHW = (_SND_tdeAudioMode)0;
            m_tdeAudioModeDARE = (_SND_tdeAudioMode)1;
            return 0;
    }
}
