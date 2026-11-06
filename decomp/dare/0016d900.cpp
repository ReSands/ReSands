#include "dare.h"

int TMultiTrack::GetFirstChunk(unsigned int uiTrackID) {
    unsigned int ulNbBytesFilled;
    m_inBuffer.ulNbBytes = m_FileInfo.ulChunkSize;
    m_inBuffer.pData = m_pMultipleBuffer[m_pulBufferIndex[uiTrackID]].pBuffer;
    int lGetDataValue = m_pChild->GetDataByte(&m_inBuffer, &ulNbBytesFilled, (unsigned int)this);

    if (lGetDataValue == -1) {
        SND_fn_vDisplayError(8, "First chunk : Problem getting data from source!");
    }
    if (lGetDataValue == 6) {
        m_lIncompleteTrack = uiTrackID;
        m_bBufferIncomplete = 1;
        return 6;
    }
    if (m_bBufferIncomplete != 0) {
        m_bBufferIncomplete = 0;
        m_lIncompleteTrack = -1;
    }
    UpdatePositions(uiTrackID);
    return lGetDataValue;
}
