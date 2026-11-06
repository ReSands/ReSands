#include "dare.h"

extern void SND_fn_vFreeSnd(void* ptr);

CVirtualPlayer::~CVirtualPlayer()
{
    if (m_pHwModuleInfo != 0) {
        SND_fn_vFreeSnd(m_pHwModuleInfo);
        m_pHwModuleInfo = 0;
    }
}
