#include "dare.h"

extern int g_StreamCount;

void SStream::RemoveStream(void) {
    if (m_bUsingStream != 0) {
        g_StreamCount--;
        m_bUsingStream = 0;
    }
}
