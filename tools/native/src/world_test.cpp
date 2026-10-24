// pop_world_test: the runtime side of POP_NATIVE_WORLD (tools/overrides/pop_native_world.cpp, compiled in
// unchanged) without the game: a frame from `pop_state_frame <state> <world.pnf> --world` goes through
// submitFrame and the GS hook gets the marker packet the game's DMA list carries; the hook's two image
// uploads (frame, Z) are checked against the picture and GS Z of `pop_native_view <world.pnf> ... --gsz`.
//
//   pop_world_test <world.pnf> <out prefix> [frames]
// writes <prefix>.rgba (640x448 CT32 bytes) and <prefix>.gsz (uint32 per pixel), prints timings.
// Build: needs a runtime with the GS host-inject hook (patch 38): -DPOP_SOTC_SRC=<PS2Recomp-sotc>.

#include "../../overrides/pop_native_world.cpp"

#include <fstream>
#include <tuple>

namespace
{
    struct Upload
    {
        uint64_t bitbltbuf, trxreg;
        std::vector<uint8_t> data;
    };
    std::vector<Upload> g_uploads;
    GSHostInject::Hook g_hook = nullptr;
}

// stand-ins for the runtime's GS (only what the hook calls)
void GS::uploadImageNative(uint64_t bitbltbuf, uint64_t, uint64_t trxreg, uint64_t, const uint8_t *data, uint32_t sizeBytes)
{
    g_uploads.push_back({bitbltbuf, trxreg, std::vector<uint8_t>(data, data + sizeBytes)});
}
void GSHostInject::setHook(Hook hook) { g_hook = hook; }

int main(int argc, char **argv)
{
    if (argc < 3)
    {
        std::fprintf(stderr, "usage: pop_world_test <world.pnf> <out prefix> [frames]\n");
        return 2;
    }
    std::ifstream f(argv[1], std::ios::binary);
    std::vector<uint8_t> pnf((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    const int frames = argc > 3 ? std::atoi(argv[3]) : 3;
    if (!pop_native_world::active(nullptr) || !g_hook)
    {
        std::fprintf(stderr, "not active\n");
        return 1;
    }
    alignas(64) static unsigned char gsStorage[1 << 16]; // the hook only passes the GS on
    GS &gs = *reinterpret_cast<GS *>(gsStorage);
    GSHostInject::Targets t;
    t.fbp = 0x8C; // the fight save's FRAME / ZBUF (tools/native/world_check.py)
    t.fbw = 10;
    t.fpsm = 0;
    t.zbp = 0x118;
    t.zpsm = 0x31;
    // a GIF packet that is not the marker must pass through
    const uint32_t other[8] = {0x00008001u, 0x10000000u, 0xEu, 0u, 1u, 0u, 0x4Cu, 0u};
    if (g_hook(gs, reinterpret_cast<const uint8_t *>(other), 32, t))
    {
        std::fprintf(stderr, "FAIL: a non-marker packet was consumed\n");
        return 1;
    }
    // later frames as the game sends them: <world.pnf>.2 if pop_state_frame wrote one (POP_STATE_WORLD_SECOND:
    // PNF7, batches by key), else the first frame without its texture pixels (PNF6: key, w = h = 0)
    std::vector<uint8_t> again;
    std::ifstream second(std::string(argv[1]) + ".2", std::ios::binary);
    if (second)
        again.assign((std::istreambuf_iterator<char>(second)), std::istreambuf_iterator<char>());
    else
    {
        size_t o = 4 + 64;
        uint32_t ni;
        std::memcpy(&ni, &pnf[o], 4);
        again.assign(pnf.begin(), pnf.begin() + o + 4);
        o += 4;
        for (uint32_t i = 0; i < ni; ++i)
        {
            uint32_t kwh[3];
            std::memcpy(kwh, &pnf[o], 12);
            const uint32_t stub[3] = {kwh[0], 0, 0};
            again.insert(again.end(), reinterpret_cast<const uint8_t *>(stub), reinterpret_cast<const uint8_t *>(stub) + 12);
            o += 12 + size_t(kwh[1]) * kwh[2] * 4;
        }
        again.insert(again.end(), pnf.begin() + o, pnf.end());
    }
    uint32_t fullGeneration = 0; // as the game: a frame written in a new kept-mesh generation sends all its geometry
    for (int n = 1; n <= frames; ++n)
    {
        g_uploads.clear();
        const auto t0 = std::chrono::steady_clock::now();
        const uint32_t generation = pop_native_world::sentGeneration();
        const bool full = generation != fullGeneration;
        fullGeneration = generation;
        pop_native_world::submitFrame(uint32_t(n), generation, std::vector<uint8_t>(full ? pnf : again));
        // the marker as pop_native_live.cpp writes it (after the CNT tag): GIF tag, A+D "PNWD" + n -> reg 0x7F
        const uint32_t marker[8] = {0x00008001u, 0x10000000u, 0xEu, 0u, uint32_t(n), 0x504E5744u, 0x7Fu, 0u};
        const bool consumed = g_hook(gs, reinterpret_cast<const uint8_t *>(marker), 32, t);
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        std::printf("frame %d: consumed %d, %zu uploads, %.1f ms from submit to uploads", n, int(consumed), g_uploads.size(), ms);
        if (!consumed || g_uploads.size() != 2)
            return 1;
        double sum = 0;
        for (size_t i = 0; i < g_uploads[0].data.size(); i += 4)
            sum += g_uploads[0].data[i] + g_uploads[0].data[i + 1] + g_uploads[0].data[i + 2];
        std::printf(", %s, picture mean %.1f\n", full ? "full" : "kept", sum / (3.0 * g_uploads[0].data.size() / 4));
        for (const Upload &u : g_uploads)
            std::printf("  BITBLTBUF %016llx (DBP %llx DBW %llu DPSM %02llx) TRXREG %016llx, %zu bytes\n", (unsigned long long)u.bitbltbuf,
                        (unsigned long long)((u.bitbltbuf >> 32) & 0x3FFF), (unsigned long long)((u.bitbltbuf >> 48) & 0x3F),
                        (unsigned long long)((u.bitbltbuf >> 56) & 0x3F), (unsigned long long)u.trxreg, u.data.size());
    }
    {
        using pop_native_world::g_stats;
        const double n = double(g_stats.drawn);
        std::printf("worker per frame: %.2f ms (read %.2f, upload %.2f, draw + readback %.2f)\n", g_stats.drawMs / n, g_stats.loadMs / n,
                    g_stats.uploadMs / n, g_stats.renderMs / n);
    }
    // the last frame's uploads: colour as is, Z unpacked from 24 bits
    std::ofstream(std::string(argv[2]) + ".rgba", std::ios::binary)
        .write(reinterpret_cast<const char *>(g_uploads[0].data.data()), std::streamsize(g_uploads[0].data.size()));
    std::vector<uint32_t> z(640 * 448);
    for (size_t i = 0; i < z.size(); ++i)
        z[i] = g_uploads[1].data[3 * i] | (g_uploads[1].data[3 * i + 1] << 8) | (g_uploads[1].data[3 * i + 2] << 16);
    std::ofstream(std::string(argv[2]) + ".gsz", std::ios::binary).write(reinterpret_cast<const char *>(z.data()), std::streamsize(z.size() * 4));
    // a marker whose frame never comes: consumed after the wait, nothing uploaded
    g_uploads.clear();
    const uint32_t late[8] = {0x00008001u, 0x10000000u, 0xEu, 0u, 999u, 0x504E5744u, 0x7Fu, 0u};
    const bool consumed = g_hook(gs, reinterpret_cast<const uint8_t *>(late), 32, t);
    std::printf("missing frame: consumed %d, %zu uploads\n", int(consumed), g_uploads.size());
    return consumed && g_uploads.empty() ? 0 : 1;
}
