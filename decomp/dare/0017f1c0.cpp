#include "dare.h"

extern int gs_rDefaultDistSatur;
extern int gs_rDefaultDistBackGround;
extern int gs_rDefaultDistTransitLinear;
extern int gs_rDefaultDistInaudible;

void SND_fn_vGetDefaultRollOff(_RollOffParam*pstRollOff) {
    pstRollOff->rDistSatur = gs_rDefaultDistSatur;
    pstRollOff->rDistBackGround = gs_rDefaultDistBackGround;
    pstRollOff->rDistTransitLinear = gs_rDefaultDistTransitLinear;
    pstRollOff->rDistInaudible = gs_rDefaultDistInaudible;
}
