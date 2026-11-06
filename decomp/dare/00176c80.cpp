#include "dare.h"

extern _tdstBankSetList gs_BankSetList;

void SND_fn_vResetBankSetList(void) {
    SND_fn_vEnterCriticalSectionThreadSnd_();
    SND_fn_vMouchardThreadsnd(__FILE__, 0x5cc);
    if (gs_BankSetList.hFirstElementOpt != 0) {
        SND_fn_vFreeSnd(gs_BankSetList.hFirstElementOpt);
    }
    gs_BankSetList.lNumberOfElementsOpt = 0;
    gs_BankSetList.hFirstElementOpt = 0;
    SND_fn_vQuitCriticalSectionThreadSnd();
}
