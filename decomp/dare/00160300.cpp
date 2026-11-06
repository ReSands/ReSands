#include "dare.h"

extern int iRam00680c80;
extern unsigned int uRam00680c90;
extern void SND_fn_vFreeSnd(unsigned int);

void SND_fn_vDesInitStack(void) {
    volatile int* base = (volatile int*)0x680000;
    if (base[0x320] != 0) {
        SND_fn_vFreeSnd(base[0x324]);
    }
}
