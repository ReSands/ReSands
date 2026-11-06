#include "dare.h"

TMultiTrack::~TMultiTrack() {
    if (m_inBuffer.pData) {
        m_inBuffer.pData = 0;
    }
    if (m_pulBufferIndex) {
        SND_fn_vFreeSnd(m_pulBufferIndex);
        m_pulBufferIndex = 0;
    }
    if (m_pbEOF) {
        SND_fn_vFreeSnd(m_pbEOF);
        m_pbEOF = 0;
    }
    if (m_pstSubChunkInfo) {
        for (unsigned int i = 0; i < m_ulNbBuffer; i++) {
            if (m_pstSubChunkInfo[i]) {
                SND_fn_vFreeSnd(m_pstSubChunkInfo[i]);
                m_pstSubChunkInfo[i] = 0;
            }
        }
        SND_fn_vFreeSnd(m_pstSubChunkInfo);
        m_pstSubChunkInfo = 0;
    }
    if (m_pMultipleBuffer) {
        SND_fn_vFreeSnd(m_pMultipleBuffer);
        m_pMultipleBuffer = 0;
    }
    if (m_pMT_Array) {
        SND_fn_vFreeSnd(m_pMT_Array);
        m_pMT_Array = 0;
    }
    if (m_pAllBuffer) {
        SND_fn_vFreeSnd(m_pAllBuffer);
        m_pAllBuffer = 0;
    }
    if (m_paHeaderExtraData) {
        SND_fn_vFreeSnd(m_paHeaderExtraData);
        m_paHeaderExtraData = 0;
    }
    if (m_paHeaderTracksBufferPos) {
        SND_fn_vFreeSnd(m_paHeaderTracksBufferPos);
        m_paHeaderTracksBufferPos = 0;
    }
    if (m_paHeaderTracksBuffer) {
        SND_fn_vFreeSnd(m_paHeaderTracksBuffer);
        m_paHeaderTracksBuffer = 0;
    }
}
