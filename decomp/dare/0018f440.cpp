#include "dare.h"

extern int sprintf(char *, const char *, ...);
extern void SND_fn_vDisplayErrorEx(int, char *, int);

void CASyncBankLoader::TestBankHeader(void) {
    char czErrMsg[1024];
    int lMinor;
    int lMedium;
    int lMajor;

    SND_fn_vGetSoundEngineVersion(&lMinor, &lMedium, &lMajor);

    if (m_pstBankHeader->lBankVersion != ((lMinor << 16) + lMedium)) {
        sprintf(czErrMsg, "The version of the bank %s is incorrect.", m_pBank->czBankName);
        SND_fn_vDisplayErrorEx(9, czErrMsg, 3);
        { __typeof__((_tdeLoadSoundBankState)0x13) perm_t320 = (_tdeLoadSoundBankState)0x13; m_eLoadingState = perm_t320; }
    } else {
        m_pBank->pBankHeader = m_pstBankHeader;
        m_eLoadingState = (_tdeLoadSoundBankState)5;
        ReadBankEvents();
    }
}
