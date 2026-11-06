#include "dare.h"

void ThemePath::SetPublicSndParam(_SoundParam *pstPublicSoundParam) {
    SND_fn_vMemCopySnd(&m_stPublicSoundParam, pstPublicSoundParam, 0x58);
    if (m_pOneShotPath != 0) {
        m_pOneShotPath->SetPublicSndParam(&m_stPublicSoundParam);
    }
}
