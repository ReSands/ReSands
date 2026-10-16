// pop_postfx_gpu_judge: runs a GPU (compute shader) port of a post effect and compares its output
// with the CPU reference in postfx.h on real frames. Used to judge local-model ports.
//   pop_postfx_gpu_judge sprite    <shader.spv> <frame.png> [<frame.png> ...]
//   pop_postfx_gpu_judge diffusion <shader.spv> <frame.png> [<frame.png> ...]
// A frame's glow mask is read from <frame>_alpha.png when present (else alpha 0).
// Prints one report line per case, then "RESULT ok|fail <cases ok> <cases>". Exit 0 when all pass.
//
// sprite: the shader draws one GS sprite (pop_postfx::gsSprite) into `dst`. Bindings: 0 = src,
//   1 = dst, both uint arrays of packed RGBA8 (r = bits 0-7). Push constants (std430):
//   { vec4 xy; vec4 uv; ivec4 size /* srcW, srcH, dstW, dstH */; uint colour; uint flags; }
//   flags bit 0 = textured, bit 1 = blend 0x44 (else copy). One invocation per destination pixel,
//   local size 8x8, dispatched over the whole destination. The host runs AE_ProcessSmartBlur's 11
//   blits with it (self-reads get a snapshot copy as src), at scale 1 and 2.
// diffusion: ColorDiffusion's four kernels on float RGBA planes (vec4 arrays, 0..255). Bindings:
//   0 = src, 1 = dst, 2 = aux. Push: { int op; int sw; int sh; int dw; int dh; int strength; float k; }
//   op 0 RESAMPLE dst(dw x dh) = bilinear(src(sw x sh)) at ((x+0.5)*sw/dw, (y+0.5)*sh/dh)
//   op 1 MASK     dst.rgb = min(255, dst.rgb * aux.r / 255)            (all dw x dh)
//   op 2 CONVOLVE dst = convolve(src, strength)                         (sw x sh = dw x dh)
//   op 3 ADD_UP   dst.rgb = min(255, dst.rgb + bilinear(src(sw x sh) at ((x+0.5)*sw/dw, ..)) * k)
//   Local size 8x8 over dw x dh. The host runs colorDiffusion's sequence with them.
#include "gpu_compute.h"
#include "postfx.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>

#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

namespace
{
    bool loadFrame(const std::string &path, std::vector<uint8_t> &rgba, int &w, int &h)
    {
        int n = 0;
        unsigned char *p = stbi_load(path.c_str(), &w, &h, &n, 4);
        if (!p)
            return false;
        rgba.assign(p, p + size_t(w) * h * 4);
        stbi_image_free(p);
        std::string ap = path.substr(0, path.size() - 4) + "_alpha.png";
        int aw = 0, ah = 0;
        unsigned char *a = stbi_load(ap.c_str(), &aw, &ah, &n, 1);
        for (size_t i = 0; i < size_t(w) * h; ++i)
            rgba[4 * i + 3] = (a && aw == w && ah == h) ? a[i] : 0;
        if (a)
            stbi_image_free(a);
        return true;
    }

    // 2x nearest upscale (a scale-2 test input)
    std::vector<uint8_t> upscale2(const std::vector<uint8_t> &in, int w, int h)
    {
        std::vector<uint8_t> o(size_t(w) * h * 16);
        for (int y = 0; y < 2 * h; ++y)
            for (int x = 0; x < 2 * w; ++x)
                std::memcpy(&o[4 * (size_t(y) * 2 * w + x)], &in[4 * (size_t(y / 2) * w + x / 2)], 4);
        return o;
    }

    struct Diff
    {
        double mean = 0;
        int max = 0;
        size_t bad = 0, n = 0;
        int bx = -1, by = -1, bc = 0, want = 0, got = 0;
    };
    Diff diff(const std::vector<uint8_t> &want, const std::vector<uint8_t> &got, int w, int tolerance)
    {
        Diff d;
        double sum = 0;
        for (size_t i = 0; i < want.size(); ++i)
        {
            if (i % 4 == 3)
                continue;
            const int e = std::abs(int(want[i]) - int(got[i]));
            sum += e;
            ++d.n;
            if (e > tolerance)
            {
                if (!d.bad)
                {
                    d.bx = int((i / 4) % size_t(w));
                    d.by = int((i / 4) / size_t(w));
                    d.bc = int(i % 4);
                    d.want = want[i];
                    d.got = got[i];
                }
                ++d.bad;
            }
            d.max = std::max(d.max, e);
        }
        d.mean = d.n ? sum / double(d.n) : 0;
        return d;
    }

    std::string describe(const char *what, const Diff &d, int w)
    {
        (void)w;
        char b[512];
        if (!d.bad)
            std::snprintf(b, sizeof b, "%s: ok (mean %.4f, max %d)", what, d.mean, d.max);
        else
            std::snprintf(b, sizeof b, "%s: %zu of %zu channel values differ by more than the tolerance (mean %.4f, max %d); "
                                       "first at pixel (%d, %d) channel %c: expected %d, got %d",
                          what, d.bad, d.n, d.mean, d.max, d.bx, d.by, "rgb"[d.bc], d.want, d.got);
        return b;
    }

    struct SpritePush
    {
        float xy[4], uv[4];
        int32_t size[4];
        uint32_t colour, flags;
    };

    // ---- sprite: SmartBlur on the GPU, one dispatch per blit ----
    bool runSprite(pop_gpu::Compute &gpu, int pipe, std::vector<uint8_t> frame, int w, int h, uint32_t colour, float fade, int scale,
                   std::vector<uint8_t> &outFrame, std::vector<uint8_t> &outWork, std::string &error)
    {
        const int ww = 448 * scale, wh = 288 * scale;
        const int fb = gpu.createBuffer(frame.size()), wb = gpu.createBuffer(size_t(ww) * wh * 4), snap = gpu.createBuffer(std::max(frame.size(), size_t(ww) * wh * 4));
        if (fb < 0 || wb < 0 || snap < 0)
        {
            error = "buffer allocation failed";
            return false;
        }
        std::memcpy(gpu.data(fb), frame.data(), frame.size());
        const float k = float(scale) / 16.0f;
        const int a = int(float(colour >> 24) * (1.0f - fade));
        const pop_postfx::SmartBlurBlit *bl = pop_postfx::smartBlurBlits();
        for (int i = 0; i < 11; ++i)
        {
            const auto &s = bl[i];
            const int dst = s.toFrame ? fb : wb;
            const int dw = s.toFrame ? w : ww, dh = s.toFrame ? h : wh;
            int src = s.fromFrame ? fb : wb;
            int sw = s.fromFrame ? w : ww, sh = s.fromFrame ? h : wh;
            if (s.textured && src == dst)
            {
                std::memcpy(gpu.data(snap), gpu.data(dst), size_t(dw) * dh * 4);
                src = snap;
            }
            SpritePush p{};
            p.xy[0] = (s.x0 - 0x6C00) * k;
            p.xy[1] = (s.y0 - 0x7200) * k;
            p.xy[2] = (s.x1 - 0x6C00) * k;
            p.xy[3] = (s.y1 - 0x7200) * k;
            p.uv[0] = s.u0 * k;
            p.uv[1] = s.v0 * k;
            p.uv[2] = s.u1 * k;
            p.uv[3] = s.v1 * k;
            p.size[0] = sw;
            p.size[1] = sh;
            p.size[2] = dw;
            p.size[3] = dh;
            p.colour = s.colour == 0xFFFFFFFFu ? ((colour & 0xFFFFFFu) | (uint32_t(a & 0xFF) << 24)) : s.colour;
            p.flags = (s.textured ? 1u : 0u) | (s.blend ? 2u : 0u);
            if (!gpu.dispatch(pipe, {s.textured ? src : snap, dst}, &p, sizeof p, uint32_t((dw + 7) / 8), uint32_t((dh + 7) / 8), 1, error))
                return false;
        }
        outFrame.assign(static_cast<uint8_t *>(gpu.data(fb)), static_cast<uint8_t *>(gpu.data(fb)) + frame.size());
        outWork.assign(static_cast<uint8_t *>(gpu.data(wb)), static_cast<uint8_t *>(gpu.data(wb)) + size_t(ww) * wh * 4);
        return true;
    }

    struct DiffPush
    {
        int32_t op, sw, sh, dw, dh, strength;
        float k;
    };

    // ---- diffusion: ColorDiffusion on the GPU, the host sequence of colorDiffusion() ----
    bool runDiffusion(pop_gpu::Compute &gpu, int pipe, const std::vector<uint8_t> &frame, int w, int h, int levels, int fix, bool mask,
                      bool convolution, int strength, std::vector<uint8_t> &out, std::string &error)
    {
        auto plane = [&](int pw, int ph) { return gpu.createBuffer(size_t(pw) * ph * 16); };
        auto fp = [&](int b) { return static_cast<float *>(gpu.data(b)); };
        const int full = plane(w, h), alpha = plane(w, h), dummy = plane(1, 1);
        for (size_t i = 0; i < size_t(w) * h; ++i)
        {
            for (int c = 0; c < 3; ++c)
                fp(full)[4 * i + c] = frame[4 * i + c];
            fp(alpha)[4 * i] = fp(alpha)[4 * i + 1] = fp(alpha)[4 * i + 2] = frame[4 * i + 3];
        }
        auto run = [&](int op, int src, int sw, int sh, int dst, int dw, int dh, int aux, float kk) {
            DiffPush p{op, sw, sh, dw, dh, strength, kk};
            return gpu.dispatch(pipe, {src, dst, aux}, &p, sizeof p, uint32_t((dw + 7) / 8), uint32_t((dh + 7) / 8), 1, error);
        };
        int lw = w / 2, lh = h / 2;
        int level = plane(lw, lh);
        if (!run(0, full, w, h, level, lw, lh, dummy, 0))
            return false;
        if (mask)
        {
            const int ah = plane(lw, lh);
            if (!run(0, alpha, w, h, ah, lw, lh, dummy, 0) || !run(1, dummy, lw, lh, level, lw, lh, ah, 0))
                return false;
        }
        struct L
        {
            int b, w, h;
        };
        std::vector<L> pyramid;
        for (int l = 1; l < levels && lw >= 2 && lh >= 2; ++l)
        {
            const int nw = lw / 2, nh = lh / 2, next = plane(nw, nh);
            if (!run(0, level, lw, lh, next, nw, nh, dummy, 0))
                return false;
            level = next;
            lw = nw;
            lh = nh;
            if (convolution)
            {
                const int conv = plane(lw, lh);
                if (!run(2, level, lw, lh, conv, lw, lh, dummy, 0))
                    return false;
                level = conv;
            }
            pyramid.push_back({level, lw, lh});
        }
        const float k = float(fix & 0xFF) / 128.0f;
        for (int l = int(pyramid.size()) - 1; l >= 0; --l)
            if (!run(3, pyramid[size_t(l)].b, pyramid[size_t(l)].w, pyramid[size_t(l)].h, full, w, h, dummy, k))
                return false;
        out = frame;
        for (size_t i = 0; i < size_t(w) * h; ++i)
            for (int c = 0; c < 3; ++c)
                out[4 * i + c] = uint8_t(std::clamp(fp(full)[4 * i + c], 0.0f, 255.0f));
        return true;
    }
}

int main(int argc, char **argv)
{
    if (argc < 4)
    {
        std::fprintf(stderr, "usage: pop_postfx_gpu_judge sprite|diffusion <shader.spv> <frame.png> [...]\n");
        return 2;
    }
    const std::string task = argv[1];
    std::vector<uint32_t> spirv;
    if (!pop_gpu::readSpirv(argv[2], spirv))
    {
        std::printf("cannot read SPIR-V %s\nRESULT fail 0 1\n", argv[2]);
        return 1;
    }
    pop_gpu::Compute gpu;
    std::string error;
    if (!gpu.init(error))
    {
        std::printf("GPU init: %s\nRESULT fail 0 1\n", error.c_str());
        return 1;
    }
    const int pipe = gpu.createPipeline(spirv, task == "sprite" ? 2 : 3, error);
    if (pipe < 0)
    {
        std::printf("pipeline: %s\nRESULT fail 0 1\n", error.c_str());
        return 1;
    }
    int cases = 0, ok = 0;
    for (int i = 3; i < argc; ++i)
    {
        std::vector<uint8_t> frame;
        int w = 0, h = 0;
        if (!loadFrame(argv[i], frame, w, h))
        {
            std::printf("cannot read %s\n", argv[i]);
            continue;
        }
        const char *name = std::strrchr(argv[i], '/') ? std::strrchr(argv[i], '/') + 1 : argv[i];
        if (task == "sprite")
        {
            struct Case
            {
                uint32_t colour;
                float fade;
                int scale;
            } cs[] = {{0x2A808080u, 0.0f, 1}, {0x60A06040u, 0.3f, 1}, {0x2A808080u, 0.0f, 2}};
            for (const Case &c : cs)
            {
                std::vector<uint8_t> in = c.scale == 2 ? upscale2(frame, w, h) : frame;
                const int fw = w * c.scale, fh = h * c.scale;
                pop_postfx::Surface sf, sw;
                sf.w = fw;
                sf.h = fh;
                sf.rgba = in;
                pop_postfx::smartBlur(sf, sw, c.colour, c.fade, 11, false, c.scale);
                std::vector<uint8_t> gf, gw;
                char what[256];
                std::snprintf(what, sizeof what, "%s colour %08X fade %.1f scale %d", name, c.colour, double(c.fade), c.scale);
                ++cases;
                if (!runSprite(gpu, pipe, in, fw, fh, c.colour, c.fade, c.scale, gf, gw, error))
                {
                    std::printf("%s: GPU error: %s\n", what, error.c_str());
                    continue;
                }
                const Diff dw = diff(sw.rgba, gw, sw.w, 1), df = diff(sf.rgba, gf, fw, 1);
                const bool pass = !dw.bad && !df.bad;
                ok += pass;
                std::printf("%s\n", describe((std::string(what) + ", work buffer").c_str(), dw, sw.w).c_str());
                std::printf("%s\n", describe((std::string(what) + ", frame").c_str(), df, fw).c_str());
            }
        }
        else
        {
            struct Case
            {
                int levels, fix;
                bool mask, conv;
                int strength;
                bool synthMask; // replace the frame's alpha by its green channel (a mask that is never empty)
            } cs[] = {{6, 128, true, true, 4, false},  {4, 64, false, false, 0, false}, {5, 200, true, false, 20, false},
                      {6, 128, false, true, 4, false}, {6, 96, true, true, 9, true},     {3, 255, true, true, 0, true}};
            for (const Case &c : cs)
            {
                std::vector<uint8_t> in = frame;
                if (c.synthMask)
                    for (size_t j = 0; j < in.size(); j += 4)
                        in[j + 3] = in[j + 1];
                std::vector<uint8_t> want = in, got;
                pop_postfx::colorDiffusion(want, w, h, c.levels, c.fix, c.mask, c.conv, c.strength);
                char what[256];
                std::snprintf(what, sizeof what, "%s levels %d fix %d mask %d%s convolution %d strength %d", name, c.levels, c.fix, c.mask,
                              c.synthMask ? " (mask = green)" : "", c.conv, c.strength);
                ++cases;
                if (!runDiffusion(gpu, pipe, in, w, h, c.levels, c.fix, c.mask, c.conv, c.strength, got, error))
                {
                    std::printf("%s: GPU error: %s\n", what, error.c_str());
                    continue;
                }
                // float sums in another order can land on the other side of an integer: allow 2
                const Diff d = diff(want, got, w, 2);
                const bool pass = !d.bad && d.mean < 0.05;
                ok += pass;
                std::printf("%s%s\n", describe(what, d, w).c_str(), (!d.bad && !pass) ? " (but mean error too high)" : "");
            }
        }
    }
    std::printf("RESULT %s %d %d\n", ok == cases && cases ? "ok" : "fail", ok, cases);
    return ok == cases && cases ? 0 : 1;
}
