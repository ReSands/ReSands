#include "dare.h"

void ThemePath::RefreshPublicParam(void) {
    if (m_pOneShotPath != 0) {
        m_pOneShotPath->SetPublicSndParam(&m_stPublicSoundParam);
    }
}
