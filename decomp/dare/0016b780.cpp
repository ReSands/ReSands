#include "dare.h"

_SND_tdeAudioMode RPS2::GetAudioModeForTool(void) {
    switch (m_stdeAudioModeHW) {
        case SND_E_DOLBYPL2:
            return SND_E_DOLBYPL2;
        case SND_E_STEREO:
            return SND_E_STEREO;
        default:
            m_stdeAudioModeHW = (_SND_tdeAudioMode)SND_E_STEREO;
            return SND_E_STEREO;
    }
}
