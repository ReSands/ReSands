#include "dare.h"

extern void SND_fn_vDisplayError(int, const char*);

int TMultiTrack::FileIsValid(void) {
    if (m_FileInfo.ulVersion != 4) {
        goto error;
    }
    if (m_FileInfo.ulNbTrack != m_ulNbTrack) {
        goto error;
    }
    if (!(m_FileInfo.ulFileSize > m_inBuffer.ulNbBytes)) {
        goto error;
    }
    if (m_FileInfo.ulLastChunkID == 0) {
        goto error;
    }
    if (m_FileInfo.ulChunkHeaderSize != (m_ulNbTrack + 3) * 4) {
        goto error;
    }
    if (m_FileInfo.ulChunkSize == 0) {
        goto error;
    }
    if (m_FileInfo.ulExtraHeaderSize == 0) {
        goto error;
    }
    if (m_FileInfo.ulMinSubChunkSize == 0) {
        goto error;
    }
    return 1;

error:
    if (m_FileInfo.ulVersion != 4) {
        SND_fn_vDisplayError(0xd, "You have an old MultiTrack version. Rebuild your MultiTrack files!");
    }
    return 0;
}
