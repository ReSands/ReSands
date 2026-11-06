#include "dare.h"

extern int g_eInitSxd;

void SND_fn_vDesInitSnd(void) {
    SND_fn_vDesInitSeq();
    SND_fn_vDesInitMultiTrackSxd();
    SND_fn_vDesInitTheme();
    if (g_eInitSxd == 1) {
        SND_fn_vDesInitSxd();
    }
}
