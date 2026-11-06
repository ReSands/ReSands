#include "dare.h"

extern int SND_fn_lGetSpuBiggestReverbSize(void);

unsigned int SND_fn_ulGetSPUHighestAddressWav(void) {
    if (SND_fn_bIsReverbEnabled() != 0) {
        return 0x1FFFFF - SND_fn_lGetSpuBiggestReverbSize();
    }
    return 0x1FFF7F;
}
