#include "dare.h"

int TVirtualSample::Resume() {
    if (m_eState != C_STOPPED) {
        m_eState = C_PLAYING;
        return 1;
    }
    return 0;
}
