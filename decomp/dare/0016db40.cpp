#include "dare.h"

int TMultiTrack::ChangeChild(CTool *pNewChild, int) {
    CTool *pOldTool = m_pChild;
    if (this->RemoveChild(0) == 0) {
        return 0;
    }
    if (this->AddChild(pNewChild) == -1) {
        this->AddChild(pOldTool);
        return 0;
    }
    return 1;
}
