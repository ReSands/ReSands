#include "dare.h"

void SND_fn_vSndRealTwinToSndReal(int rtwin, int* pr1, int* pr2) {
    if (pr1) {
        *pr1 = (rtwin >> 16) << 8;
    }
    if (pr2) {
        *pr2 = (rtwin << 16) >> 8;
    }
}
