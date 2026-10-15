// Judge driver for the movie header parser: prints the canonical text reference.py prints.
// Usage: driver <file>   Not written by the model.
#include "movie.h"
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
using namespace pop_movie;
int main(int argc, char **argv)
{
    if (argc < 2)
        return 2;
    std::ifstream f(argv[1], std::ios::binary);
    std::vector<uint8_t> d((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    MovieHeader h;
    if (!parseMovieHeader(d.data(), d.size(), h))
    {
        std::printf("X\n");
        return 0;
    }
    uint32_t fb;
    std::memcpy(&fb, &h.fps, 4);
    std::printf("fps %08x\nchunking %u %u %u\nflags %u %u %u %u\nsizes %u %u %u %u\nchunks", fb, h.framesPerChunk, h.chunkCount,
                h.trackCount, h.hasVideo, h.hasAlpha, h.hasSound, h.hasSubtitles, h.videoHeaderSize, h.alphaHeaderSize,
                h.soundHeaderSize, h.subtitleSize);
    for (size_t i = 0; i < h.chunks.size(); ++i)
        std::printf(" %x", h.chunks[i]);
    std::printf("\nvideo %u mb %u %u frames %u\ninfo", h.videoVersion, h.widthMb, h.heightMb, h.frameCount);
    for (uint32_t x : h.movieInfo)
        std::printf(" %x", x);
    std::printf("\nindex");
    for (uint32_t x : h.frameIndex)
        std::printf(" %x", x);
    std::printf("\n");
    for (const SoundTrack &t : h.tracks)
    {
        std::printf("track");
        for (uint32_t x : t.fields)
            std::printf(" %x", x);
        std::printf(" |");
        for (uint32_t x : t.subChunks)
            std::printf(" %x", x);
        std::printf("\n");
    }
    std::printf("data %x\n", h.dataStart);
    return 0;
}
