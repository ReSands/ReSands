#include "dare.h"

int RPS2::bIsAudioModeHWSupported(_SND_tdeAudioMode eAudioMode)
{
    if (eAudioMode == SND_E_STEREO || eAudioMode == SND_E_DOLBYPL2)
        return 1;
    return 0;
}
