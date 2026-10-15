// PS2 movie (.INT) header parsing (native movie decoder, stage 1). Fixed API: the implementation
// (movie.cpp) must give exactly the Python reference's results (tools/llm_port/movie/reference.py)
// on every movie of the disc. Format: docs/movies.md. All values little-endian.
#pragma once
#include <cstdint>
#include <cstddef>
#include <vector>

namespace pop_movie
{
    struct SoundTrack
    {
        uint32_t fields[5];              // the 0x14 header bytes as 5 u32 (+4: 1, +8: bits 16, +0xC: channels, +0x10: rate)
        std::vector<uint32_t> subChunks; // chunk count + 1 sub-chunk positions
    };

    struct MovieHeader
    {
        float fps = 0;
        uint32_t framesPerChunk = 0;    // +0x08
        uint32_t chunkCount = 0;        // +0x0C (N)
        uint32_t trackCount = 0;        // +0x10 (T)
        uint8_t hasVideo = 0, hasAlpha = 0, hasSound = 0, hasSubtitles = 0; // +0x14..+0x17
        uint32_t videoHeaderSize = 0, alphaHeaderSize = 0, soundHeaderSize = 0, subtitleSize = 0; // +0x18..+0x24
        std::vector<uint32_t> chunks;   // N + 1 chunk positions (from 0x28)
        uint32_t videoVersion = 0;      // first u32 of the video header (3)
        uint32_t movieInfo[32] = {};    // the 0x80-byte VID movie info as 32 u32
        uint32_t widthMb = 0, heightMb = 0, frameCount = 0; // movieInfo[0x60/4], [0x64/4], [0x70/4]
        std::vector<uint32_t> frameIndex; // frameCount entries, after the movie info (if movieInfo[0x6C/4] != 0)
        std::vector<SoundTrack> tracks;   // T tracks, each soundHeaderSize bytes, after video + alpha headers
        uint32_t dataStart = 0;           // file offset of chunk data: after all headers and subtitles
    };

    // Returns false when the buffer is too short for what the header says, or the video version
    // is not 3 while hasVideo is set. Never reads outside [0, len).
    bool parseMovieHeader(const uint8_t *d, size_t len, MovieHeader &out);
}
