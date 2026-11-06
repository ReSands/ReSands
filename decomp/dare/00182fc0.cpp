#include "dare.h"
static char init;
static SND_ClockNative clock;
void SND_fn_vResumeTimer(int lTimerId) {
    _tdstSndTimer *pSndTimer = (_tdstSndTimer *)lTimerId;
    if (lTimerId == 0 || lTimerId == -2) return;
    if (pSndTimer->bPaused == 0) return;
    pSndTimer->bPaused = 0;
    if (init == 0) {
        clock.m_current = 0;
        clock.m_frequency = 0x11940000;
        clock.m_previous = SND_fn_ulGetCount();
        init = 1;
    }
    pSndTimer->initTick += (clock.m_current + (unsigned int)(SND_fn_ulGetCount() - clock.m_previous)) - pSndTimer->pauseTick;
}
