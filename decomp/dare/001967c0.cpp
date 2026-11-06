#include "dare.h"

extern char g_61[24];

int SND_fn_bCanFreeSubBlock(int iSubBlockIndex) {
    switch (iSubBlockIndex & 0xFF) {
        case 0:
        case 3:
        case 1:
            return 0;
        default:
            SND_fn_vDisplayError(0, g_61);
            return 0;
    }
}
