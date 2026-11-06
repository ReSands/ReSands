#include "dare.h"

void TMultiTrack::SetPreviousIndexBuffer(unsigned int *pulBufferIndex) {
    if (*pulBufferIndex == 0) {
        *pulBufferIndex = *(unsigned int*)((char*)this + 0x50) - 1;
    } else {
        *pulBufferIndex = *pulBufferIndex - 1;
    }
}
