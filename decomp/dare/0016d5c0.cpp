#include "dare.h"

void TMultiTrack::InitArray(void) {
    for (unsigned int i = 0; i < m_ulNbTrack; i++) {
        m_pMT_Array[i].ulParentID = 0;
        m_pMT_Array[i].pData = 0;
    }
}
