// inprobe: which VU data quadwords a VU1 program's output depends on (22 Oct; finds where a program reads its
// lights). Replays one recorded run (PS2X_VU1_TRACE) through the runtime's VU1 interpreter, then once per
// quadword in [from, to) with that quadword's four floats changed (x1.5 + 7), and prints per quadword how many
// output words (the GS packets the run writes) change, with the recorded values. Output words that are small
// integers (<= 0xFFFF) count apart: vertex colours and UVs land there, positions and tags do not.
//   inprobe <trace> <image dir> <image hash> [--run N (Nth run of that image, default 0)] [--from Q] [--to Q]
//           [--match W (hex): count only runs whose own uploads hold the word W, e.g. a mesh's first position]
//           [--dump: print the quadwords in [from, to) as recorded instead of probing]
//           [--out: print the run's output words (4 per line, hex and float) instead of probing]
#define main vu1replay_main
#include "replay.cpp"
#undef main

int main(int argc, char **argv)
{
    if (argc < 4)
    {
        std::fprintf(stderr, "usage: inprobe <trace> <image dir> <image hash> [--run N] [--from Q] [--to Q]\n");
        return 2;
    }
    const uint64_t want = std::stoull(argv[3], nullptr, 16);
    long nth = 0, from = 0, to = 0x80;
    uint32_t match = 0;
    bool matching = false, dump = false, outOnly = false;
    for (int i = 4; i < argc; ++i)
    {
        dump |= !std::strcmp(argv[i], "--dump");
        outOnly |= !std::strcmp(argv[i], "--out");
    }
    for (int i = 4; i + 1 < argc; ++i)
    {
        if (!std::strcmp(argv[i], "--run"))
            nth = std::stol(argv[i + 1]);
        else if (!std::strcmp(argv[i], "--from"))
            from = std::stol(argv[i + 1], nullptr, 0);
        else if (!std::strcmp(argv[i], "--to"))
            to = std::stol(argv[i + 1], nullptr, 0);
        else if (!std::strcmp(argv[i], "--match"))
            match = uint32_t(std::stoul(argv[i + 1], nullptr, 16)), matching = true;
    }
    std::ifstream in(argv[1], std::ios::binary);
    std::vector<uint8_t> data(0x4000, 0), code;
    {
        char name[64];
        std::snprintf(name, sizeof name, "/vu1_%016llx.bin", (unsigned long long)want);
        std::ifstream c(std::string(argv[2]) + name, std::ios::binary);
        code.assign(std::istreambuf_iterator<char>(c), {});
        if (code.size() != 0x4000)
        {
            std::fprintf(stderr, "image %s not found\n", name);
            return 1;
        }
    }
    auto words = [](const Run &r) {
        std::vector<uint32_t> w;
        for (const auto &p : r.packets)
            for (size_t i = 0; i + 4 <= p.size(); i += 4)
            {
                uint32_t v;
                std::memcpy(&v, &p[i], 4);
                w.push_back(v);
            }
        return w;
    };
    long seen = 0;
    for (uint64_t rec = 0;; ++rec)
    {
        uint32_t magic = 0, maxCycles = 0;
        uint64_t hash = 0, tick = 0;
        if (!in.read(reinterpret_cast<char *>(&magic), 4) || magic != 0x52543156u)
            break;
        VU1InterpreterAccess::TraceState state;
        in.read(reinterpret_cast<char *>(&maxCycles), 4);
        in.read(reinterpret_cast<char *>(&hash), 8);
        in.read(reinterpret_cast<char *>(&tick), 8);
        in.read(reinterpret_cast<char *>(&state), sizeof state);
        bool hit = !matching;
        for (;;)
        {
            uint16_t h[2];
            in.read(reinterpret_cast<char *>(h), 4);
            if (!in || h[0] == 0xFFFFu)
                break;
            in.read(reinterpret_cast<char *>(data.data() + size_t(h[0]) * 16), std::streamsize(h[1]) * 16);
            for (size_t i = 0; i < size_t(h[1]) * 4 && !hit; ++i)
                hit = std::memcmp(&data[size_t(h[0]) * 16 + 4 * i], &match, 4) == 0;
        }
        if (hash != want || !hit || seen++ != nth)
            continue;
        if (dump)
        {
            for (long q = from; q < to && q < 0x400; ++q)
            {
                uint32_t u[4];
                std::memcpy(u, &data[size_t(q) * 16], 16);
                std::printf("  qw 0x%03lx: %08x %08x %08x %08x\n", q, u[0], u[1], u[2], u[3]);
            }
            return 0;
        }
        const auto base = words(replay(state, code, data, maxCycles));
        if (outOnly)
        {
            for (size_t i = 0; i + 4 <= base.size(); i += 4)
            {
                float f[4];
                std::memcpy(f, &base[i], 16);
                std::printf("  %4zu: %08x %08x %08x %08x  %g %g %g %g\n", i / 4, base[i], base[i + 1], base[i + 2], base[i + 3], double(f[0]), double(f[1]),
                            double(f[2]), double(f[3]));
            }
            return 0;
        }
        size_t small = 0;
        for (uint32_t w : base)
            small += w <= 0xFFFF;
        std::printf("run %llu (record %llu): %zu output words, %zu small\n", (unsigned long long)nth, (unsigned long long)rec, base.size(), small);
        for (long q = from; q < to && q < 0x400; ++q)
        {
            std::vector<uint8_t> d = data;
            float *f = reinterpret_cast<float *>(&d[size_t(q) * 16]);
            const float *o = reinterpret_cast<const float *>(&data[size_t(q) * 16]);
            for (int l = 0; l < 4; ++l)
                f[l] = f[l] * 1.5f + 7.0f;
            const auto out = words(replay(state, code, d, maxCycles));
            size_t changed = 0, changedSmall = 0;
            for (size_t i = 0; i < std::min(out.size(), base.size()); ++i)
                if (out[i] != base[i])
                {
                    ++changed;
                    changedSmall += base[i] <= 0xFFFF;
                }
            if (out.size() != base.size() || changed)
            {
                uint32_t u[4];
                std::memcpy(u, o, 16);
                std::printf("  qw %3ld (0x%03lx): %5zu changed (%zu small)%s  [%g %g %g %g | %08x %08x %08x %08x]\n", q, q, changed, changedSmall,
                            out.size() != base.size() ? " size differs" : "", double(o[0]), double(o[1]), double(o[2]), double(o[3]), u[0], u[1], u[2], u[3]);
            }
        }
        return 0;
    }
    std::fprintf(stderr, "run %ld of image %016llx not in the trace\n", nth, (unsigned long long)want);
    return 1;
}
