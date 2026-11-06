#include "dare.h"

unsigned int TMultiTrack::GetAssociatedTrack(unsigned int ulParentID) {
    unsigned int uiTrackID = 0;
    unsigned int i = 0;
    while (uiTrackID < m_ulNbTrack) {
        if (ulParentID == m_pMT_Array[i].ulParentID) {
            return uiTrackID;
        }
        i++;
        uiTrackID++;
    }
    return -1;
}
