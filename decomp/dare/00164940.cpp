#include "dare.h"

void Destroy_LoopingPath(LoopingPath **pPath) {
    if (*pPath != 0) {
        (*pPath)->DestroyPath();
        delete *pPath;
        *pPath = 0;
    }
}
