#include "dare.h"

int TVirtualSample::Pause(void) {
    if ((unsigned char)m_eState != C_STOPPED) {
        *(unsigned char*)&m_eState = C_PAUSED;
        return 1;
    }
    return 0;
}
