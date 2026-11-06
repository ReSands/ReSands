#include "dare.h"

int SND_fn_rDivRealRealSnd(int r1, int r2) {
    return (int)(((float)r1 / (float)r2) * 65536.0f);
}
