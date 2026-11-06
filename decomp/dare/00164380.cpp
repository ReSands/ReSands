#include "dare.h"

int CPath::IsDone(void) const {
    CTool* pLastTool = GetLastTool();
    if (pLastTool != 0) {
        return pLastTool->IsDone();
    }
    return 0;
}
