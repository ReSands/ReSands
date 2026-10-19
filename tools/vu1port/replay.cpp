// vu1replay: replays VU1 runs recorded with PS2X_VU1_TRACE (runtime ps2_vu1_core.cpp traceVu1Run) through
// the runtime's own VU1 interpreter and writes each run's XGKICK output: the judge for VU1 program ports
// (tools/vu1port/). Record: 'V1TR', maxCycles, image hash, vsync tick, TraceState (as the runtime build that
// wrote it lays it out; this tool is built x86_64 so that the layout is the PC's), then VU data deltas
// against the previous record ({first qword, count} + data, ended by {0xFFFF, 0}).
//
//   vu1replay <trace> <image dir> <out> [--only <hash>] [--limit N] [--twice]
// <out>: per replayed run 'V1RO', record index, image hash, start pc, packet count, then per packet its
// byte size and bytes; then the VU data after the run (16 KB) and the end state's vi[16] (judges of a port's
// memory side effects). --only: just the runs of one image (hex); --twice: run each twice, compare (the
// interpreter must be deterministic for the judge to mean anything).
#include "ps2_vu1_core.cpp"

#include <cinttypes>
#include <cstdio>
#include <fstream>
#include <map>
#include <string>
#include <vector>

void PS2Memory::submitGifPacket(GifPathId, const uint8_t *, uint32_t, bool, bool) {}
void GS::processGIFPacket(const uint8_t *, uint32_t) {}

namespace
{
    struct Run
    {
        std::vector<std::vector<uint8_t>> packets;
        std::vector<uint8_t> dataAfter;
        int32_t vi[16];
        bool ended;
    };

    Run replay(const VU1InterpreterAccess::TraceState &state, const std::vector<uint8_t> &code, const std::vector<uint8_t> &data,
               uint32_t maxCycles)
    {
        static VU1Interpreter vu(VU1Interpreter::Unit::VU1);
        alignas(64) static unsigned char gsStorage[1 << 16]; // never touched: the sink does not forward
        GS &gs = *reinterpret_cast<GS *>(gsStorage);
        VU1InterpreterAccess::traceRestore(vu, state);
        Run run;
        run.dataAfter = data;
        std::vector<uint8_t> codeCopy = code;
        Vu1PacketSink sink;
        sink.forward = false;
        packetSink = &sink;
        run.ended = VU1InterpreterAccess::runLoopWith(vu, codeCopy.data(), uint32_t(codeCopy.size()), run.dataAfter.data(),
                                                      uint32_t(run.dataAfter.size()), gs, nullptr, maxCycles, nullptr);
        packetSink = nullptr;
        run.packets = std::move(sink.packets);
        const VU1State end = VU1InterpreterAccess::effectiveState(vu);
        for (int i = 0; i < 16; ++i)
            run.vi[i] = int32_t(end.vi[i]);
        return run;
    }
}

int main(int argc, char **argv)
{
    if (argc < 4)
    {
        std::fprintf(stderr, "usage: vu1replay <trace> <image dir> <out> [--only <hash>] [--limit N] [--twice]\n");
        return 2;
    }
    uint64_t only = 0;
    long limit = -1;
    bool twice = false;
    for (int i = 4; i < argc; ++i)
    {
        const std::string a = argv[i];
        if (a == "--only" && i + 1 < argc)
            only = std::stoull(argv[++i], nullptr, 16);
        else if (a == "--limit" && i + 1 < argc)
            limit = std::stol(argv[++i]);
        else if (a == "--twice")
            twice = true;
    }
    std::ifstream in(argv[1], std::ios::binary);
    std::ofstream out(argv[3], std::ios::binary);
    std::map<uint64_t, std::vector<uint8_t>> images;
    std::vector<uint8_t> data(0x4000, 0);
    std::map<uint64_t, uint64_t> perImage;
    uint64_t records = 0, replayed = 0, packets = 0, unended = 0, nondeterministic = 0;
    std::fprintf(stderr, "TraceState %zu bytes (VU1State %zu)\n", sizeof(VU1InterpreterAccess::TraceState), sizeof(VU1State));
    for (;;)
    {
        uint32_t magic = 0, maxCycles = 0;
        uint64_t hash = 0, tick = 0;
        if (!in.read(reinterpret_cast<char *>(&magic), 4))
            break;
        if (magic != 0x52543156u)
        {
            std::fprintf(stderr, "record %" PRIu64 ": bad magic %08x at %lld (TraceState layout differs from the recorder's?)\n", records, magic,
                         (long long)in.tellg() - 4);
            return 1;
        }
        VU1InterpreterAccess::TraceState state;
        in.read(reinterpret_cast<char *>(&maxCycles), 4);
        in.read(reinterpret_cast<char *>(&hash), 8);
        in.read(reinterpret_cast<char *>(&tick), 8);
        in.read(reinterpret_cast<char *>(&state), sizeof state);
        for (;;)
        {
            uint16_t h[2];
            in.read(reinterpret_cast<char *>(h), 4);
            if (!in || h[0] == 0xFFFFu)
                break;
            if (size_t(h[0] + h[1]) * 16 > data.size())
            {
                std::fprintf(stderr, "record %" PRIu64 ": delta out of range\n", records);
                return 1;
            }
            in.read(reinterpret_cast<char *>(data.data() + size_t(h[0]) * 16), std::streamsize(h[1]) * 16);
        }
        const uint64_t index = records++;
        ++perImage[hash];
        if ((only && hash != only) || (limit >= 0 && long(replayed) >= limit))
            continue;
        auto img = images.find(hash);
        if (img == images.end())
        {
            char name[64];
            std::snprintf(name, sizeof name, "/vu1_%016llx.bin", (unsigned long long)hash);
            std::ifstream f(std::string(argv[2]) + name, std::ios::binary);
            std::vector<uint8_t> code((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
            if (code.size() != 0x4000)
            {
                std::fprintf(stderr, "image %016llx missing\n", (unsigned long long)hash);
                return 1;
            }
            img = images.emplace(hash, std::move(code)).first;
        }
        const Run run = replay(state, img->second, data, maxCycles);
        if (twice)
        {
            const Run again = replay(state, img->second, data, maxCycles);
            if (again.packets != run.packets || again.dataAfter != run.dataAfter)
                ++nondeterministic;
        }
        ++replayed;
        unended += !run.ended;
        packets += run.packets.size();
        const uint32_t head[2] = {0x4F523156u, uint32_t(index)};
        out.write(reinterpret_cast<const char *>(head), 8);
        out.write(reinterpret_cast<const char *>(&hash), 8);
        const uint32_t pcCount[2] = {state.state.pc, uint32_t(run.packets.size())};
        out.write(reinterpret_cast<const char *>(pcCount), 8);
        for (const auto &p : run.packets)
        {
            const uint32_t n = uint32_t(p.size());
            out.write(reinterpret_cast<const char *>(&n), 4);
            out.write(reinterpret_cast<const char *>(p.data()), n);
        }
        out.write(reinterpret_cast<const char *>(run.dataAfter.data()), 0x4000);
        out.write(reinterpret_cast<const char *>(run.vi), sizeof run.vi);
    }
    std::fprintf(stderr, "%" PRIu64 " records, %" PRIu64 " replayed, %" PRIu64 " packets, %" PRIu64 " not ended within maxCycles, %" PRIu64
                         " nondeterministic\n",
                 records, replayed, packets, unended, nondeterministic);
    for (const auto &[h, n] : perImage)
        std::fprintf(stderr, "  image %016llx: %" PRIu64 " runs\n", (unsigned long long)h, n);
    return 0;
}
