#include "dare.h"

void fn_vRetObjectSoundExtraCoefDefault(int, _SND_tdstBlockEvent *, float *pitch, float *pan, float *vol) {
    if (pitch) *(int*)pitch = 0x3f800000;
    if (pan) *(int*)pan = 0x3f800000;
    if (vol) *(int*)vol = 0x3f800000;
}
