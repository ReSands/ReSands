#include "dare.h"

void SND_fn_vFreeSubBlockHW(void*pDataPtr, _tdeBlockType eBlockID) {
    switch (eBlockID) {
        case E_HARDWARE_WAV_BLOCK:
            SND_fn_vFreeSPU(pDataPtr);
            break;
        case E_HARDWARE_WAV_BLOCK_STREAM:
            SND_fn_vFreeIOP(pDataPtr);
            break;
    }
}
