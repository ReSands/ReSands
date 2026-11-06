#include "dare.h"

extern int SND_fn_bInitSourceMemoryModule__Fv(void);
extern int SND_fn_bInitSourceStreamingModule__Fv(void);

int SND_fn_bInitSourceModule(void) {
    int bRet = SND_fn_bInitSourceMemoryModule__Fv();
    if (bRet) {
        bRet = (bRet != 0);
        if (bRet) {
            bRet = SND_fn_bInitSourceStreamingModule__Fv();
            bRet = (bRet != 0);
        }
        bRet &= 0xff;
    }
    return bRet;
}
