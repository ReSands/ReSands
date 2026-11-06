#include "dare.h"

extern "C" int CreateSema(SemaParam *);
extern "C" int WaitSema(int);
// retail-only file static at 0x4e8fa0 (POP1's ThreadWorker calls DoWork instead): name not known
extern int gs_lWorkerSemaId;

void RPS2::ThreadWorker(void *pRenderer)
{
    SemaParam stSema;
    if (pRenderer == 0)
        return;
    stSema.maxCount = 1;
    stSema.initCount = 0;
    gs_lWorkerSemaId = CreateSema(&stSema);
    while (1) {
        WaitSema(gs_lWorkerSemaId);
        SND_fn_vUpdateRenderer();
    }
}
