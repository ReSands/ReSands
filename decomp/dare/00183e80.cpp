#include "dare.h"

extern char g_czSndMasterDirectory[256];
extern int FUN_00199e80(char *);
extern "C" char *strncat(char *, const char *, unsigned int);

void SND_fn_vResolveProjectFileName(char* czRelativeName, char* AbsoluteName, unsigned int ulAbsNameSize) {
    char name[256];
    char texte[1024];

    SND_fn_vMemsetSnd(AbsoluteName, 0, ulAbsNameSize);
    SND_fn_vStrncpy(name, g_czSndMasterDirectory, 0xff);
    strncat(name, czRelativeName, 0xff - strlen(name));
    if (FUN_00199e80(name) != 0) {
        SND_fn_vStrncpy(AbsoluteName, name, ulAbsNameSize - 1);
    } else {
        sprintf(texte, "%s", czRelativeName);
        SND_fn_vDisplayErrorEx(0xc, texte, 3);
        SND_fn_vStrncpy(AbsoluteName, czRelativeName, ulAbsNameSize - 1);
    }
}
