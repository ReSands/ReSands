#include "dare.h"

void TVolPan::SetVolFade(int rFadeVol_dB) {
    m_rParentFadeVol_Linear = SND_fn_rConvertdB2LinearFractionnal(rFadeVol_dB);
}
