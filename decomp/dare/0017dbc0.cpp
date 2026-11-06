#include "dare.h"

extern unsigned short FUN_00173040(void);
extern unsigned short FUN_00173240(void);

unsigned short SND_fn_uwGetResourceNbChannel(_tdstBlockResourceMem* pstBRMem) {
    if (pstBRMem == 0) {
        return 0;
    }
    switch (pstBRMem->eType) {
        case TYPE_SAMPLE:
            return FUN_00173040();
        case TYPE_MULTITRACK:
            return FUN_00173240();
        default:
            return 0;
    }
}
