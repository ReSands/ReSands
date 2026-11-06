#include "dare.h"

unsigned int SND_fn_ulGetSPULowestAddressWav(void) {
    return SND_fn_ulGetNumberOfIOPStreamVoices() * 0x4000 + 0x5010;
}
