#include "dare.h"

void OneShotMULTITRACKPath::IndexedFadeTrack(int rNewVolume_dB, int rFadeDuration, _SND_tdeFadeType eFadeType, int lTrackID, int lMTTChanID) {
    if (lTrackID < 0) return;
    if (lTrackID >= (int)m_ulNbTrack) return;
    if (m_MTTPath.pBranchPath[lTrackID] == 0) return;
    if (m_lMTTChannelID != lMTTChanID) return;
    m_MTTPath.pIndexedFader[lTrackID]->StartFade(rFadeDuration, eFadeType, rNewVolume_dB);
}
