#include "dare.h"

void OneShotMULTITRACKPath::Resume(void) const {
    if (m_pSource) m_pSource->Resume();
    if (m_pTMultiTrack) m_pTMultiTrack->Resume();
    unsigned int ulTrackIdx;
    if (m_MTTPath.pBranchPath) {
        for (ulTrackIdx = 0; ulTrackIdx < m_ulNbTrack; ulTrackIdx++) {
            if (m_MTTPath.pBranchPath[ulTrackIdx]) {
                m_MTTPath.pBranchPath[ulTrackIdx]->Resume();
            }
        }
    }
    if (m_MTTPath.pIndexedFader) {
        for (ulTrackIdx = 0; ulTrackIdx < m_ulNbTrack; ulTrackIdx++) {
            if (m_MTTPath.pIndexedFader[ulTrackIdx]) {
                m_MTTPath.pIndexedFader[ulTrackIdx]->Resume();
            }
        }
    }
    if (m_pTAdder) m_pTAdder->Resume();
    if (m_pTFinalPitch) m_pTFinalPitch->Resume();
    if (m_pTFinalVolume) m_pTFinalVolume->Resume();
    if (m_pTFinalFader) m_pTFinalFader->Resume();
}
