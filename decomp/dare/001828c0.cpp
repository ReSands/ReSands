#include "dare.h"

extern "C" int sceSifLoadModule(char *, int, int *);

void fn_vLoadModuleIRX(char *pszModuleName) {
    char szResolveModuleName[256] = {0};
    char *dir = SND_fn_szGetSoftDirectory();
    strcpy(szResolveModuleName, dir);
    if (strlen(szResolveModuleName) == 0) {
        SND_fn_vDisplayError(8, "SetSoftDirectory should be called before initializing sound engine.");
    }
    strcat(szResolveModuleName, pszModuleName);
    int ret = sceSifLoadModule(szResolveModuleName, 0, 0);
    if (ret < 0) {
        do {
            SND_fn_vDisplayError(8, "Problem while loading IRX module used by DARE sound engine, trying again...");
            ret = sceSifLoadModule(szResolveModuleName, 0, 0);
        } while (ret < 0);
    }
}
