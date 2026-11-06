#include "dare.h"

extern int gs_rDefaultDistSatur;
extern int gs_rDefaultDistBackGround;
extern int gs_rDefaultDistTransitLinear;
extern int gs_rDefaultDistInaudible;

int SND_fn_rDistanceToVolume(int _rDist) {
    _RollOffParam rolloff;
    rolloff.rDistSatur = gs_rDefaultDistSatur;
    rolloff.rDistBackGround = gs_rDefaultDistBackGround;
    rolloff.rDistTransitLinear = gs_rDefaultDistTransitLinear;
    rolloff.rDistInaudible = gs_rDefaultDistInaudible;
    return SND_fn_rDistanceToVolumeEx(_rDist, &rolloff);
}
