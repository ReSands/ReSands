#include "dare.h"

// The retail assembly sets a1 to 0 in the delay slot of the jal to SND_fn_pvMallocSnd,
// indicating the call passes a second integer argument (likely a flag or padding).
// We add an overloaded declaration to match the compiler's behavior.
extern void *SND_fn_pvMallocSnd(unsigned int size, int flags);

void* SND_fn_pvMallocSubBlock(int lMallocSize, int(*plArrayClusterId)[2], _tdeBlockType eBlockID) {
    switch(eBlockID) {
        case E_HARDWARE_WAV_BLOCK:
        case E_HARDWARE_WAV_BLOCK_CANFREE:
        case E_HARDWARE_WAV_BLOCK_STREAM:
            return SND_fn_pvMallocSubBlockHW(lMallocSize, plArrayClusterId, eBlockID);
        case E_TOOL_WAV_BLOCK:
            return SND_fn_pvMallocSnd(lMallocSize, 0);
        default:
            return 0;
    }
}
