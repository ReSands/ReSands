#include "dare.h"

void TMultiTrack::FindNbBufferNeeded(void) {
    if (m_ulMaxRequestedDataSize % m_FileInfo.ulMinSubChunkSize != 0) {
        m_ulNbBuffer = (m_ulMaxRequestedDataSize / m_FileInfo.ulMinSubChunkSize + 1) * 2;
    } else {
        m_ulNbBuffer = (m_ulMaxRequestedDataSize / m_FileInfo.ulMinSubChunkSize) * 2;
    }
    m_ulTotalBufferSize = m_ulNbBuffer * m_FileInfo.ulChunkSize;
}
