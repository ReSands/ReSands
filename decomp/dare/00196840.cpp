#include "dare.h"

void* SND_fn_pvMallocSubBlockHW(int lMallocSize, int(*plArrayClusterId)[2], _tdeBlockType eBlockID) {
    switch (eBlockID) {
        case E_HARDWARE_WAV_BLOCK:
            return SND_fn_pvMallocSPU(lMallocSize, plArrayClusterId[0][0]);
        case E_HARDWARE_WAV_BLOCK_STREAM:
            return SND_fn_pvMallocIOP(lMallocSize, plArrayClusterId[0][1]);
        default:
            return 0;
    }
}
