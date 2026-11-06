#include "dare.h"

void SND_fn_vDestroyTimer(int lTimerId)
{
    if (lTimerId == 0 || lTimerId == -2) return;
    SND_fn_vFreeSnd((void *)lTimerId);
}
