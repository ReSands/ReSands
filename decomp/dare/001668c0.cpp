#include "dare.h"

void OneShotMULTITRACKPath::SetTrackVolume(int rNewVolume_dB, int rFadeDuration, _SND_tdeFadeType eFadeType, int lTrackID, int lMTTChanID) {
    if (lTrackID >= 0 && lTrackID < (int)m_ulNbTrack) {
        BranchPath* pBranch = m_MTTPath.pBranchPath[lTrackID];
        if (pBranch && m_lMTTChannelID == lMTTChanID) {
            pBranch->StartFade(rFadeDuration, eFadeType, rNewVolume_dB);
        }
    }
}
