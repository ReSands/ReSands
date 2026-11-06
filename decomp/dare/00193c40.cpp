#include "dare.h"

CThemeChannel::~CThemeChannel() {
    if (m_lVoice != -1) {
        SND_fn_vStopTransitionSnd(m_lVoice);
    }
    m_lVoice = -1;
}
