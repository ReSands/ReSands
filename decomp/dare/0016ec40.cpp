#include "dare.h"

int TMultiTrack::Associate(unsigned int ulParentID, unsigned int uiTrackID) {
    if (ulParentID == 0 || uiTrackID >= m_ulNbTrack || AlreadyAssociated(uiTrackID)) {
        return 0;
    }
    m_pMT_Array[uiTrackID].ulParentID = ulParentID;
    return 1;
}
