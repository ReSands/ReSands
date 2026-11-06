#include "dare.h"


int fn_bGeneratePath(const char *czDirectory, const char *czFileName, char *name, unsigned int ulNameSize, int bLocalised) {
    int lLength;
    char czLang[12];

    if (czDirectory == 0 || czFileName == 0 || name == 0) {
        return 0;
    }

    SND_fn_vStrncpy(name, (char *)czDirectory, 0x100);

    if (bLocalised != 0) {
        strcpy(czLang, SND_fn_szGetCurrentLangDirectory());
        lLength = strlen(czLang);
        czLang[lLength + 1] = 0;
        czLang[lLength] = SND_fn_cGetDirectorySeparator();
        lLength = strlen(name);
        if (ulNameSize < (unsigned int)(lLength + strlen(czLang) + 1)) {
            return 0;
        }
        strcat(name, czLang);
    }

    lLength = strlen(name);
    if (ulNameSize < (unsigned int)(lLength + strlen(czFileName) + 1)) {
        return 0;
    }
    strcat(name, czFileName);

    return 1;
}
