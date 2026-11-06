#include "dare.h"

extern void SND_fn_vFreeSndAligned(void *ptr, unsigned int size);

void TAdder::DeleteBuffers(void) {
    if (m_inBuffer.pData) {
        SND_fn_vFreeSndAligned(m_inBuffer.pData, 0x10);
        m_inBuffer.pData = 0;
        m_ulinDataSize = 0;
    }
    if (m_MixBuffer32.pData) {
        SND_fn_vFreeSndAligned(m_MixBuffer32.pData, 0x10);
        m_MixBuffer32.pData = 0;
        m_ulMixBuffer32Size = 0;
    }
}
