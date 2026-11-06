#include "dare.h"

extern "C" {
_SND_tdeInit SND_fn_eInitSxdMic(void) {
    *(volatile int*)0x6a42b8 = 0;
    *(volatile int*)0x6a42c0 = 0;
    *(volatile int*)0x6a42c4 = 0;
    *(volatile int*)0x6a42c8 = 0;
    return SND_E_INIT_OK;
}
}
