#include "dare.h"

extern void DeleteSema(int);

void SND_fn_vDesInitThreadSnd(void) {
    DeleteSema(*((int*)((char*)0x004F0000 - 0x7090)));
    DeleteSema(*((int*)((char*)0x004F0000 - 0x7080)));
}
