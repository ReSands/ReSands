#include "dare.h"

int CVirtualPlayer::GetPos(int lVoiceId)
{
    int lId = *(int *)(lVoiceId + 4);
    if (lId != -1)
        return m_pstVirtFunc->pfnGetPos(lId);
    return -0x10000;
}
