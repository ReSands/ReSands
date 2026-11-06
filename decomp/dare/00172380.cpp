#include "dare.h"
int SND_fn_rGetLengthMultiTrack(_tduRefRes res)
{
    _tdstMultiTrackElement *pTrack = res.pstPtr->uRes.stMultiTrack.hFirstElementOpt;
    return SND_fn_rDivRealRealSnd(pTrack->ulNbSamples, pTrack->ulFreq);
}
