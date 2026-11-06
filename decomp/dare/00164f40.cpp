#include "dare.h"

int LoopingPath::GetPos(void) {
    int rPos;
    if (m_pOneShotPath == 0) {
        rPos = -0x10000;
    } else {
        rPos = m_pOneShotPath->GetPos();
        if (m_bLoopPartPlaying && rPos != -0x10000 && rPos != -0x20000) {
            rPos += m_rAttackSizeSample;
        }
    }
    return rPos;
}
