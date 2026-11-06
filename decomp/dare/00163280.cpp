#include "dare.h"

extern void *SND_fn_vMemCopySnd(void *, const void *, int);

int BranchPath::SetInputFormat(const _SND_tdstToolFormat *pFormat) {
    if (!IsFormatValid(pFormat)) {
        return 0;
    }
    SND_fn_vMemCopySnd(&m_stInputFormat, pFormat, 12);
    return 1;
}
