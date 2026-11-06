#include "dare.h"

int BranchPath::UpdateBranchPath(const _SND_tdstToolFormat *pInFormat, const _SND_tdstToolFormat *pOutFormat, _SND_tdeTypeZip eZip, _tdstBlockResourceMem *pRes) {
    int ret = SetInputFormat(pInFormat);
    if (ret == 0) return 0;
    ret = SetOutputFormat(pOutFormat);
    if (ret == 0) return 0;
    m_eZip = eZip;
    ret = CreateBranch(pRes);
    return ret != 0;
}
