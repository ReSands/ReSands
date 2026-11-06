#include "dare.h"

int TTransition::GetPos() {
    if (m_bLoopingPath) {
        return -65536;
    }
    return SND_fn_rDivRealRealSnd(m_lCurrentPos, m_stOutputFormat.uFormat.stPCM.ulFreq);
}
