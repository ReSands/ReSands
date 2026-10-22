// lightprobe: checks a model of the character VU1 program's point-light falloff by replaying recorded runs
// (PS2X_VU1_TRACE) through the runtime's VU1 interpreter with the light inputs changed (22 Oct). The light
// block as CVuManager::DoSetSkinLightEnv's typed path uploads it: VU 4 + i = slot vector (point light:
// position, w = type << 4 = 0), VU 16 + i = colour (floats 0..255), VU 20 + i = falloff pair (a, b), VU 29 =
// ambient. Per run with a point light in slot s: every other colour and the ambient zeroed, slot s's colour
// set to 200, then three replays: (a, b) = (1, 0) -> C1 (no falloff), (0, k) -> Ck, the recorded (a, b) -> C0.
// The output words that differ between the replays are the vertex colour channels. Model: Ck = k d^2 C1
// (gives d^2 per vertex), C0 = clamp(a + b d^2, 0, 1) C1. Prints per run the channels compared and the
// worst error; C1 <= 200 everywhere means the light direction is normalised.
//   lightprobe <trace> <image dir> <image hash> [--runs N]
#define main vu1replay_main
#include "replay.cpp"
#undef main
#include <algorithm>

int main(int argc, char **argv)
{
    if (argc < 4)
    {
        std::fprintf(stderr, "usage: lightprobe <trace> <image dir> <image hash> [--runs N]\n");
        return 2;
    }
    const uint64_t want = std::stoull(argv[3], nullptr, 16);
    long maxRuns = 20;
    for (int i = 4; i + 1 < argc; ++i)
        if (!std::strcmp(argv[i], "--runs"))
            maxRuns = std::stol(argv[i + 1]);
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
    auto qf = [](std::vector<uint8_t> &d, int qw, int lane) -> float & { return *reinterpret_cast<float *>(&d[size_t(qw) * 16 + lane * 4]); };
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
    long probed = 0;
    double worstAll = 0, maxC1 = 0;
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
        for (;;)
        {
            uint16_t h[2];
            in.read(reinterpret_cast<char *>(h), 4);
            if (!in || h[0] == 0xFFFFu)
                break;
            in.read(reinterpret_cast<char *>(data.data() + size_t(h[0]) * 16), std::streamsize(h[1]) * 16);
        }
        if (hash != want || probed >= maxRuns)
            continue;
        // a point light: slot vector w == 0 (type 0) with a position, colour, and a falloff pair with b < 0
        int slot = -1;
        for (int s = 0; s < 4 && slot < 0; ++s)
        {
            std::vector<uint8_t> &d = data;
            uint32_t w;
            std::memcpy(&w, &d[size_t(4 + s) * 16 + 12], 4);
            const float b = qf(d, 20 + s, 1);
            if ((w & 0xF0) == 0 && b < 0 && (qf(d, 16 + s, 0) + qf(d, 16 + s, 1) + qf(d, 16 + s, 2)) > 0)
                slot = s;
        }
        if (slot < 0)
            continue;
        const float a0 = qf(data, 20 + slot, 0), b0 = qf(data, 20 + slot, 1);
        std::vector<uint8_t> base = data;
        for (int s = 0; s < 4; ++s)
            for (int l = 0; l < 3; ++l)
                qf(base, 16 + s, l) = s == slot ? 200.0f : 0.0f;
        for (int l = 0; l < 3; ++l)
            qf(base, 29, l) = 0.0f;
        const float k = 1e-4f;
        auto variant = [&](float a, float b) {
            std::vector<uint8_t> d = base;
            qf(d, 20 + slot, 0) = a;
            qf(d, 20 + slot, 1) = b;
            return words(replay(state, code, d, maxCycles));
        };
        const auto c1 = variant(1, 0), ck = variant(0, k), c0 = variant(a0, b0), cz = variant(0, 0);
        static bool rawShown = false;
        if (std::getenv("LIGHTPROBE_RAW") && !rawShown)
        {
            // the shape of the falloff: a alone (0.5, 0), b alone at two scales, both
            const auto ha = variant(0.5f, 0), b1 = variant(0, 1e-3f), b2 = variant(0, 2e-3f), ab = variant(0.5f, 1e-3f), a2 = variant(2.0f, 0), an = variant(-0.5f, 0);
            int shown = 0;
            for (size_t i = 0; i < c1.size() && shown < 24; ++i)
                if (c1[i] != cz[i] && c1[i] <= 0xFFFF && int32_t(c1[i]) - int32_t(cz[i]) >= 40)
                {
                    auto v = [&](const std::vector<uint32_t> &x) { return int32_t(x[i]) - int32_t(cz[i]); };
                    std::printf("  word %zu: zero %u | a1 %d | a.5 %d | b1e-3 %d | b2e-3 %d | a.5+b1e-3 %d | recorded %d | a2 %d | a-.5 %d\n", i, cz[i], v(c1),
                                v(ha), v(b1), v(b2), v(ab), v(c0), v(a2), v(an));
                    ++shown;
                }
            rawShown = shown > 0;
        }
        if (c1.size() != ck.size() || c1.size() != c0.size() || c1.size() != cz.size())
        {
            std::printf("run %llu: packet sizes differ\n", (unsigned long long)rec);
            continue;
        }
        // colour channels: words that differ between no falloff and zero light (as small integers)
        size_t channels = 0, bad = 0;
        double worst = 0;
        float dmin = 1e30f, dmax = 0;
        for (size_t i = 0; i < c1.size(); ++i)
        {
            if (c1[i] == cz[i] || c1[i] > 0xFFFF || cz[i] > 0xFFFF)
                continue;
            const double v1 = double(int32_t(c1[i]) - int32_t(cz[i])), vk = double(int32_t(ck[i]) - int32_t(cz[i])),
                         v0 = double(int32_t(c0[i]) - int32_t(cz[i]));
            maxC1 = std::max(maxC1, v1);
            if (v1 < 20)
                continue; // too few levels to measure
            ++channels;
            const double d2 = vk / (k * v1);
            dmin = std::min(dmin, float(std::sqrt(std::max(0.0, d2))));
            dmax = std::max(dmax, float(std::sqrt(std::max(0.0, d2))));
            const double model = std::clamp(a0 + b0 * d2, 0.0, 1.0) * v1;
            const double err = std::fabs(model - v0);
            worst = std::max(worst, err);
            bad += err > 2.5;
        }
        worstAll = std::max(worstAll, worst);
        std::printf("run %llu slot %d (a %.4f b %.6f): %zu channels, distance %.2f-%.2f, model off by > 2.5 on %zu, worst %.2f\n",
                    (unsigned long long)rec, slot, double(a0), double(b0), channels, double(dmin), double(dmax), bad, worst);
        ++probed;
    }
    std::printf("%ld runs probed; worst error %.2f levels; largest no-falloff contribution %.0f (light colour 200)\n", probed, worstAll, maxC1);
    return 0;
}
