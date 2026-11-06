#include "dare.h"

void SND_fn_vFreeSubBlock(void*pDataPtr, _tdeBlockType eBlockID) {
    switch(eBlockID) {
        case E_HARDWARE_WAV_BLOCK:
        case E_HARDWARE_WAV_BLOCK_CANFREE:
        case E_HARDWARE_WAV_BLOCK_STREAM:
            SND_fn_vFreeSubBlockHW(pDataPtr, eBlockID);
            break;
        case E_TOOL_WAV_BLOCK:
            if (pDataPtr != 0) {
                SND_fn_vFreeSnd(pDataPtr);
            }
            break;
    }
}
