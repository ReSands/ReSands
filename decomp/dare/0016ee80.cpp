#include "dare.h"

void TMultiTrack::SetNextIndexBuffer(unsigned int *pulBufferIndex) {
    *pulBufferIndex = *pulBufferIndex + 1;
    if (*pulBufferIndex >= *(unsigned int*)((char*)this + 0x50)) {
        *pulBufferIndex = 0;
    }
}
