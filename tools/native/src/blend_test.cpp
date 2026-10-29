// pop_blend_test: one quad per blend kind (docs/renderer_audit.md item 17). Each case goes through
// materialFromGsFlags and renderOffscreen over a known destination; the pixel is checked against the GS
// formula ((A - B) * C >> 7) + D (COLCLAMP) worked out from the ALPHA register SetBlending (0x4B2500) sets
// for that kind (table: game/analysis/native/decomp/004b2500.c, every branch). Exit 0 = all pass. As and Ad stay <= 0x80 (above it: an open question, audit item 9).
#include "native_renderer.h"

#include <cstdio>
#include <cstdlib>

namespace
{
    // destination: clear colour (Cd) and its alpha (Ad, 0x80 = 1.0)
    const int kCd[3] = {60, 120, 200}, kAd = 0x30;

    struct Case
    {
        const char *name;
        uint32_t flags;  // render state flags (blend kind in bits 16-19, 0x80 inverse, 0x1000 destination alpha)
        uint8_t alpha;   // the GS ALPHA register the game sets for it: A bits 0-1, B 2-3, C 4-5, D 6-7
        int fix;
    };
    const Case kCases[] = {
        {"opaque", 0x00000, 0x29, 0},
        {"blend", 0x10000, 0x44, 0},
        {"blend inverse", 0x10080, 0x01, 0},
        {"blend dest alpha", 0x11000, 0x54, 0},
        {"blend inverse, dest", 0x11080, 0x11, 0},
        {"add dest", 0x20000, 0x09, 0},
        {"add dest, dest alpha", 0x21000, 0x19, 0},
        {"multiply", 0x30000, 0x89, 0},
        {"multiply inverse", 0x30080, 0x46, 0},
        {"multiply dest alpha", 0x31000, 0x99, 0},
        {"multiply inv, dest", 0x31080, 0x56, 0},
        {"add", 0x40000, 0x68, 0x80},
        {"subtract", 0x50000, 0x62, 0x30},
        {"add alpha", 0x60000, 0x48, 0},
        {"add alpha, dest alpha", 0x61000, 0x58, 0},
        {"kind F", 0xF0000, 0x46, 0},
    };

    int gsBlend(uint8_t alpha, int fix, int cs, int as, int cd, int ad)
    {
        const int abd[3] = {cs, cd, 0}, c[3] = {as, ad, fix};
        const int a = abd[alpha & 3], b = abd[(alpha >> 2) & 3], cc = c[(alpha >> 4) & 3], d = abd[(alpha >> 6) & 3];
        const int v = (((a - b) * cc) >> 7) + d; // arithmetic shift, as the GS
        return v < 0 ? 0 : v > 255 ? 255 : v;
    }
}

int main()
{
    pop_native::Options o;
    for (int i = 0; i < 3; ++i)
        o.clearColour[i] = kCd[i] / 255.0f;
    o.clearColour[3] = kAd / 128.0f;
    pop_native::Renderer r;
    std::string error;
    if (!r.init(o, error))
    {
        std::fprintf(stderr, "init: %s\n", error.c_str());
        return 2;
    }
    const int sources[][4] = {{200, 100, 40, 0x60}, {255, 30, 128, 0x20}}; // Cs, As
    const float identity[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    int failed = 0;
    for (const Case &k : kCases)
        for (const auto &s : sources)
        {
            r.reset();
            pop_native::Material m;
            pop_native::materialFromGsFlags(k.flags, 0, m);
            pop_native::Mesh mesh;
            mesh.material = r.addMaterial(m);
            // full-screen quad, white texture: Cs = vertex colour; z 0.95 keeps depth inside 0..w
            const float col[4] = {s[0] / 255.0f, s[1] / 255.0f, s[2] / 255.0f, s[3] / 128.0f};
            for (int v = 0; v < 4; ++v)
            {
                pop_native::Vertex vx{{v & 1 ? 1.0f : -1.0f, v & 2 ? 1.0f : -1.0f, 0.95f}, {0, 0}, {col[0], col[1], col[2], col[3]}, {0, 0}};
                mesh.vertices.push_back(vx);
            }
            mesh.indices = {0, 1, 2, 2, 1, 3};
            pop_native::Draw d{};
            d.mesh = r.addMesh(mesh);
            std::copy(identity, identity + 16, d.model);
            std::vector<uint8_t> px;
            if (!r.upload(error) || !r.renderOffscreen(4, 4, identity, {d}, px, error))
            {
                std::fprintf(stderr, "%s: %s\n", k.name, error.c_str());
                return 2;
            }
            const uint8_t *p = &px[(2 * 4 + 2) * 4];
            bool ok = true;
            int want[3];
            for (int i = 0; i < 3; ++i)
            {
                want[i] = gsBlend(k.alpha, k.fix, s[i], s[3], kCd[i], kAd);
                ok &= std::abs(p[i] - want[i]) <= 2; // float blending vs the GS's integer steps
            }
            std::printf("%-22s Cs %3d %3d %3d As %02X: got %3d %3d %3d, GS %3d %3d %3d %s\n", k.name, s[0], s[1], s[2], s[3],
                        p[0], p[1], p[2], want[0], want[1], want[2], ok ? "ok" : "WRONG");
            failed += !ok;
        }
    {
        // mid-frame copy (Draw::copyFrame, the water's refraction): red with alpha 0x40 on the left half, then the right half
        // blended (0x44) from the copy of the left half: colour and alpha must come through, the left half must survive
        r.reset();
        pop_native::Material red, fromCopy;
        red.writeAlpha = true;
        pop_native::materialFromGsFlags(0x10000, 0, fromCopy);
        fromCopy.baseImage = pop_native::kFrameCopyImage;
        const auto quad = [&](float x0, float u1, const float col[4], int material) {
            pop_native::Mesh mesh;
            mesh.material = material;
            for (int v = 0; v < 4; ++v)
                mesh.vertices.push_back({{v & 1 ? x0 + 1.0f : x0, v & 2 ? 1.0f : -1.0f, 0.95f}, {v & 1 ? u1 : 0.0f, v & 2 ? 1.0f : 0.0f},
                                         {col[0], col[1], col[2], col[3]}, {0, 0}});
            mesh.indices = {0, 1, 2, 2, 1, 3};
            pop_native::Draw d{};
            d.mesh = r.addMesh(mesh);
            std::copy(identity, identity + 16, d.model);
            return d;
        };
        const float redCol[4] = {1, 0, 0, 0x40 / 128.0f}, white[4] = {1, 1, 1, 1};
        std::vector<pop_native::Draw> draws = {quad(-1.0f, 0.0f, redCol, r.addMaterial(red)), quad(0.0f, 0.5f, white, r.addMaterial(fromCopy))};
        draws[1].copyFrame = true;
        std::vector<uint8_t> px;
        if (!r.upload(error) || !r.renderOffscreen(8, 4, identity, draws, px, error))
        {
            std::fprintf(stderr, "frame copy: %s\n", error.c_str());
            return 2;
        }
        const uint8_t *left = &px[(2 * 8 + 1) * 4], *right = &px[(2 * 8 + 6) * 4];
        const int cs[3] = {255, 0, 0};
        bool ok = left[0] == 255 && left[1] == 0 && left[2] == 0;
        int want[3];
        for (int i = 0; i < 3; ++i)
        {
            want[i] = gsBlend(0x44, 0, cs[i], 0x40, kCd[i], kAd);
            ok &= std::abs(right[i] - want[i]) <= 2;
        }
        std::printf("%-22s left %3d %3d %3d, right %3d %3d %3d, GS %3d %3d %3d %s\n", "frame copy", left[0], left[1], left[2], right[0], right[1],
                    right[2], want[0], want[1], want[2], ok ? "ok" : "WRONG");
        failed += !ok;
    }
    std::printf("%d wrong\n", failed);
    return failed ? 1 : 0;
}
