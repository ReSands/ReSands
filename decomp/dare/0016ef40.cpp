#include "dare.h"

extern void* SND_fn_pvReAllocSnd(void* pv, unsigned int ulSize);

int TMultiTrack::UpdateParams(unsigned int _ulNbTrack) {
    if (_ulNbTrack < 2) {
        return 0;
    }
    if (_ulNbTrack != m_ulNbTrack) {
        m_ulNbTrack = _ulNbTrack;
        m_pulBufferIndex = (unsigned int*)SND_fn_pvReAllocSnd(m_pulBufferIndex, m_ulNbTrack << 2);
        m_pbEOF = (int*)SND_fn_pvReAllocSnd(m_pbEOF, m_ulNbTrack << 2);
        m_pMT_Array = (_MT_tdstArrayElement*)SND_fn_pvReAllocSnd(m_pMT_Array, m_ulNbTrack << 3);
    }
    m_bStructInitialized = 0;
    m_bFirstChunkGot = 0;
    m_bFileIsNotValid = 0;
    m_bBufferIncomplete = 0;
    m_bMainHeaderValid = 0;
    m_ulLastChunkID = 0;
    m_lPosBytes = 0;
    m_lIncompleteTrack = -1;
    for (unsigned int i = 0; i < m_ulNbTrack; i++) {
        m_pMT_Array[i].ulParentID = 0;
        m_pMT_Array[i].pData = 0;
    }
    return 1;
}
