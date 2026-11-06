#include "dare.h"

static char init;
static SND_ClockNative clock;

void SND_fn_vPauseTimer(int lTimerId)
{
    if (lTimerId == 0 || lTimerId == -2)
        return;
    if (*(int *)lTimerId != 0)
        return;

    if (!init)
    {
        clock.m_current = 0;
        clock.m_frequency = 0x11940000;
        clock.m_previous = SND_fn_ulGetCount();
        init = 1;
    }

    _tdstSndTimer *pSndTimer = (_tdstSndTimer *)lTimerId;
    pSndTimer->pauseTick = (long)(clock.m_current + (unsigned int)(SND_fn_ulGetCount() - clock.m_previous));
    pSndTimer->bPaused = 1;
}
