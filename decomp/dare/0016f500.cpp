#include "dare.h"
void TPCM::SetPos(int rPos)
{
    m_pChild->SetPosByte(SND_fn_rMulRealRealSnd(rPos, m_stOutputFormat.uFormat.stPCM.ulFreq) << m_ulShift);
}
