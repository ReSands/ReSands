#include "dare.h"

int TMultiTrack::DesAssociate(unsigned int ulParentID) {
    for (unsigned int i = 0; i < this->m_ulNbTrack; ++i) {
        if (this->m_pMT_Array[i].ulParentID == ulParentID) {
            this->m_pMT_Array[i].ulParentID = 0;
            return 1;
        }
    }
    return 0;
}
