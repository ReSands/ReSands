#include "dare.h"

// retail 0x174100 takes one argument (it writes '1' there and returns 1); POP1 has fn_bGetMapsFileName(char *, int)
extern int fn_bGetMapsFileName(char *szMapsFileName);
extern int SND_fn_hCreateStreamAsyncSnd(char* czFileName, int bStream);

void CASyncBankLoader::OpenBank(void) {
    char czFullBankName[16];
    char czBankName_Ext[128];
    char czErrMsg[1024];

    SND_fn_vStrncpy(czFullBankName, m_pBank->czBankName, 0x10);
    strcat(czFullBankName, ".SBX");
    fn_bGetMapsFileName(&czFullBankName[strlen(czFullBankName) - 1]);
    SND_fn_vResolveFileName(czFullBankName, czBankName_Ext, 0x80);
    m_stAsyncRead.hBankFile = SND_fn_hCreateStreamAsyncSnd(czBankName_Ext, -2);

    if (m_stAsyncRead.hBankFile == -1) {
        sprintf(czErrMsg, "Problem while opening the bank file %s.", m_pBank->czBankName);
        SND_fn_vDisplayErrorEx(0xc, czErrMsg, 3);
        m_eLoadingState = (_tdeLoadSoundBankState)0x13;
    } else {
        m_eLoadingState = (_tdeLoadSoundBankState)2;
    }
}
