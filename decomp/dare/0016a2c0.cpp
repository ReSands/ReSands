#include "dare.h"

extern int PollSema(int);
extern void SignalSema(int);
extern void TerminateThread(void);
extern void DeleteThread(int);
extern void DeleteSema(void);
extern int DAT_004e8fa0;
extern int iRam006a1d10;

void RPS2::DeleteUpdateBufferThread(void) {
    if (DAT_004e8fa0 != PollSema(DAT_004e8fa0)) {
        SignalSema(DAT_004e8fa0);
    }
    if (this->m_lRPS2ThreadId != -1) {
        TerminateThread();
        DeleteThread(this->m_lRPS2ThreadId);
        this->m_lRPS2ThreadId = -1;
    }
    if (DAT_004e8fa0 != -1) {
        DeleteSema();
        DAT_004e8fa0 = -1;
    }
    if (iRam006a1d10 == 1 && this->m_intHandlerId != -1) {
        this->m_intHandlerId = -1;
    }
}
