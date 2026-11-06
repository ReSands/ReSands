#include "dare.h"

int TMultiTrack::GetPosByte(void) {
    if (m_pChild->GetPosByte() == -2.0f) return 0xfffe0000;
    return m_lPosBytes;
}
