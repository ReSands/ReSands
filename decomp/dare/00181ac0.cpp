#include "dare.h"

extern unsigned int gs_ulEEInvalidVoiceArray;
extern void* gs_pstEEInvalidVoiceArrayNotAligned;
extern void* gs_pstEEInvalidVoiceArray;

void SND_fn_vDesInitSxdWave(void) {
    SND_fn_vSynchroSxdWave();
    gs_ulEEInvalidVoiceArray = 0;
    if (gs_pstEEInvalidVoiceArrayNotAligned != 0) {
        SND_fn_vFreeSnd(gs_pstEEInvalidVoiceArrayNotAligned);
    }
    gs_pstEEInvalidVoiceArrayNotAligned = 0;
    gs_pstEEInvalidVoiceArray = 0;
    SND_fn_vDesInitStack();
    SND_fn_iIOPCommandRPC(1, 0, 0, 0, 0);
    SND_fn_vDesInitFastIOPMem();
    SND_fn_vDesInitSPUMem();
    SND_fn_vDesInitIOPMem();
    g_bInitSoundDone = 0;
}
