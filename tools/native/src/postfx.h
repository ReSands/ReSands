// Post-processing passes of the game's CAfterEffect, as CPU reference implementations on the
// rendered frame (RGBA8, alpha = the glow mask the frame's draws wrote). Written from the
// decompiles (game/analysis/post/decomp/0046f680.c ColorDiffusion, decomp_proto/0046fbc0.c
// ApplyConvolution) and the GS semantics of each blit: Cs = texel * vertex colour / 128, blends
// clamped to 0..255 after every pass (COLCLAMP). A GPU version should be judged against these.
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace pop_postfx
{
    struct Plane
    {
        int w = 0, h = 0;
        std::vector<float> rgb; // 3 per pixel, 0..255
        float at(int x, int y, int c) const
        {
            x = std::clamp(x, 0, w - 1);
            y = std::clamp(y, 0, h - 1);
            return rgb[3 * (size_t(y) * w + x) + c];
        }
        // GS bilinear sample at texel coordinates (u, v) (texel centres at +0.5)
        float bilinear(float u, float v, int c) const
        {
            u -= 0.5f;
            v -= 0.5f;
            const int x0 = int(std::floor(u)), y0 = int(std::floor(v));
            const float fx = u - x0, fy = v - y0;
            return (at(x0, y0, c) * (1 - fx) + at(x0 + 1, y0, c) * fx) * (1 - fy) +
                   (at(x0, y0 + 1, c) * (1 - fx) + at(x0 + 1, y0 + 1, c) * fx) * fy;
        }
    };

    // A RectBlitter sprite from src (whole plane) to a w x h target, bilinear
    inline Plane resample(const Plane &src, int w, int h)
    {
        Plane o;
        o.w = w;
        o.h = h;
        o.rgb.resize(size_t(w) * h * 3);
        const float sx = float(src.w) / w, sy = float(src.h) / h;
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x)
                for (int c = 0; c < 3; ++c)
                    o.rgb[3 * (size_t(y) * w + x) + c] = src.bilinear((x + 0.5f) * sx, (y + 0.5f) * sy, c);
        return o;
    }

    // ApplyConvolution filter 0: every tap of the 3x3 kernel (all weights 15 in the retail table at
    // 0x63F418) plus `strength`, as the vertex colour on the 0x80 = 1.0 scale; the centre tap is a
    // copy (ALPHA 0x29), the eight neighbours (offsets of one pixel) are added (0x68, FIX 0x80)
    inline Plane convolve(const Plane &src, int strength)
    {
        const float wgt = float(std::clamp(15 + strength, 0, 255)) / 128.0f;
        Plane o = src;
        for (size_t i = 0; i < o.rgb.size(); ++i)
            o.rgb[i] = std::min(255.0f, src.rgb[i] * wgt);
        for (int dy = -1; dy <= 1; ++dy)
            for (int dx = -1; dx <= 1; ++dx)
            {
                if (!dx && !dy)
                    continue;
                for (int y = 0; y < o.h; ++y)
                    for (int x = 0; x < o.w; ++x)
                        for (int c = 0; c < 3; ++c)
                        {
                            float &d = o.rgb[3 * (size_t(y) * o.w + x) + c];
                            d = std::min(255.0f, d + src.at(x + dx, y + dy, c) * wgt);
                        }
            }
        return o;
    }

    // CAfterEffect::ColorDiffusion(levels, fix, mask, convolution, strength) on a width x height
    // RGBA8 frame (in place): half-resolution copy of Cs (times the frame alpha As/0x80 when `mask`),
    // then levels-1 halvings (bilinear), each blurred by the 3x3 box when `convolution`; levels
    // levels-1 .. 1 are then added onto the frame, upsampled bilinearly, times FIX/128.
    inline void colorDiffusion(std::vector<uint8_t> &rgba, int width, int height, int levels, int fix, bool mask, bool convolution,
                               int strength)
    {
        if (levels < 2 || width < 4 || height < 4)
            return;
        Plane full;
        full.w = width;
        full.h = height;
        full.rgb.resize(size_t(width) * height * 3);
        std::vector<float> alpha(size_t(width) * height);
        for (size_t i = 0; i < size_t(width) * height; ++i)
        {
            for (int c = 0; c < 3; ++c)
                full.rgb[3 * i + c] = rgba[4 * i + c];
            alpha[i] = rgba[4 * i + 3]; // 255 = 0x80 = 1.0
        }
        // level 0: Cs * As (ALPHA 0x88) - the alpha is filtered with the colour
        Plane level = resample(full, width / 2, height / 2);
        if (mask)
        {
            Plane a;
            a.w = width;
            a.h = height;
            a.rgb.resize(full.rgb.size());
            for (size_t i = 0; i < alpha.size(); ++i)
                a.rgb[3 * i] = a.rgb[3 * i + 1] = a.rgb[3 * i + 2] = alpha[i];
            const Plane ah = resample(a, width / 2, height / 2);
            for (size_t i = 0; i < level.rgb.size(); ++i)
                level.rgb[i] = std::min(255.0f, level.rgb[i] * ah.rgb[i] / 255.0f);
        }
        std::vector<Plane> pyramid;
        for (int l = 1; l < levels && level.w >= 2 && level.h >= 2; ++l)
        {
            level = resample(level, level.w / 2, level.h / 2);
            if (convolution)
                level = convolve(level, strength);
            pyramid.push_back(level);
        }
        const float k = float(fix & 0xFF) / 128.0f;
        for (int l = int(pyramid.size()) - 1; l >= 0; --l)
        {
            const Plane up = resample(pyramid[size_t(l)], width, height);
            for (size_t i = 0; i < size_t(width) * height; ++i)
                for (int c = 0; c < 3; ++c)
                    full.rgb[3 * i + c] = std::min(255.0f, full.rgb[3 * i + c] + up.rgb[3 * i + c] * k);
        }
        for (size_t i = 0; i < size_t(width) * height; ++i)
            for (int c = 0; c < 3; ++c)
                rgba[4 * i + c] = uint8_t(full.rgb[3 * i + c]);
    }

    // ---- GS-exact sprite blits (for effects whose passes read and write overlapping regions) ----

    // An RGBA8 buffer as the GS sees it (FBMSK 0xFF000000 in the blits below: alpha never written)
    struct Surface
    {
        int w = 0, h = 0;
        std::vector<uint8_t> rgba;
        uint8_t texel(int x, int y, int c) const
        {
            x = std::clamp(x, 0, w - 1);
            y = std::clamp(y, 0, h - 1);
            return rgba[4 * (size_t(y) * w + x) + c];
        }
    };

    // One SPRITE as the GS draws it (rules of the CPU rasteriser the reference frames come from):
    // pixel i covered when x0 <= i < x1, UVs (texels, FST) interpolated at the integer pixel position
    // and rounded to 1/16, bilinear with texel centres at +0.5, texture function MODULATE
    // (T * V >> 7), TCC 0 (As = vertex alpha), then ALPHA 0x29 (Cs) or 0x44 ((Cs - Cd) * As >> 7 + Cd),
    // COLCLAMP. src == nullptr: untextured (flat vertex colour). A pass that reads its own target reads
    // it as it was before the pass (a snapshot): closer to the reference frames than reading pixels
    // already written (blits 9-10 of the haze: mean error 1.7/2.4 vs 2.5/3.3), and what a GPU version
    // does anyway. The real GS's texture cache sits somewhere in between.
    enum class Blend { Copy, Lerp44 };
    inline void gsSprite(Surface &dst, const Surface *src, float x0, float y0, float x1, float y1, float u0, float v0, float u1,
                         float v1, uint32_t colour, Blend blend)
    {
        const int vr = colour & 0xFF, vg = (colour >> 8) & 0xFF, vb = (colour >> 16) & 0xFF, va = colour >> 24;
        const int px0 = std::max(0, int(std::ceil(x0))), px1 = std::min(dst.w - 1, int(std::ceil(x1)) - 1);
        const int py0 = std::max(0, int(std::ceil(y0))), py1 = std::min(dst.h - 1, int(std::ceil(y1)) - 1);
        const float du = x1 > x0 ? (u1 - u0) / (x1 - x0) : 0.0f, dv = y1 > y0 ? (v1 - v0) / (y1 - y0) : 0.0f;
        Surface snapshot;
        if (src == &dst)
        {
            snapshot = dst;
            src = &snapshot;
        }
        for (int y = py0; y <= py1; ++y)
        {
            const float tv = std::lround((v0 + (float(y) - y0) * dv) * 16.0f) / 16.0f - 0.5f;
            const int ty = int(std::floor(tv));
            const float fy = tv - float(ty);
            for (int x = px0; x <= px1; ++x)
            {
                int cs[3] = {vr, vg, vb};
                if (src)
                {
                    const float tu = std::lround((u0 + (float(x) - x0) * du) * 16.0f) / 16.0f - 0.5f;
                    const int tx = int(std::floor(tu));
                    const float fx = tu - float(tx);
                    for (int c = 0; c < 3; ++c)
                    {
                        const float t = (src->texel(tx, ty, c) * (1 - fx) + src->texel(tx + 1, ty, c) * fx) * (1 - fy) +
                                        (src->texel(tx, ty + 1, c) * (1 - fx) + src->texel(tx + 1, ty + 1, c) * fx) * fy;
                        cs[c] = std::min(255, (int(t) * cs[c]) >> 7);
                    }
                }
                uint8_t *d = &dst.rgba[4 * (size_t(y) * dst.w + x)];
                for (int c = 0; c < 3; ++c)
                    d[c] = uint8_t(blend == Blend::Copy ? cs[c] : std::clamp(((cs[c] - int(d[c])) * va >> 7) + int(d[c]), 0, 255));
            }
        }
    }

    // CAfterEffect::AE_ProcessSmartBlur (prototype 0x466240) with AE_BlurBackBufferIntoZBuffer
    // (prototype 0x4666C0): the haze. `work` = the half-res work buffer (FBP 280, shares memory with
    // Z), at least 448x288. Positions are the decompile's 12.4 values minus the XYOFFSET
    // (1728, 1824); UVs are 12.4 texels. `colour` = effect object +0x124 (RGB + strength in the top
    // byte), `fade` = +0x144 (0 when no fade runs). Not modelled: the half-width mode (flag
    // 0x2000000 of the word at prototype 0x539254).
    struct SmartBlurBlit
    {
        bool toFrame, fromFrame, textured, blend;
        int x0, y0, x1, y1, u0, v0, u1, v1;
        uint32_t colour; // 0xFFFFFFFF = the effect colour
    };
    inline const SmartBlurBlit *smartBlurBlits()
    {
        static const SmartBlurBlit b[11] = {
            // AE_BlurBackBufferIntoZBuffer, ALPHA 0x29 (Cs)
            {false, true, true, false, 0x6C00, 0x7200, 0x7C00, 0x8200, 0, 0, 0x2800, 0x1C00, 0x80808080u}, // 640x448 -> 256x256
            {false, false, false, false, 0x7C00, 0x7200, 0x8800, 0x7E00, 0, 0, 0, 0, 0u},                   // black border
            {false, false, true, false, 0x7E00, 0x7400, 0x8600, 0x7C00, 0, 0, 0x1000, 0x1000, 0x80808080u}, // -> 128 at (288,32)
            {false, false, false, false, 0x7E00, 0x7E00, 0x8400, 0x8400, 0, 0, 0, 0, 0u},                   // black border
            {false, false, true, false, 0x7EF0, 0x7EF0, 0x82F0, 0x82F0, 0x1200, 0x200, 0x1A00, 0xA00, 0x80808080u}, // -> 64
            // ALPHA 0x44 with vertex alpha 0x40: half-strength blends with one-pixel shifts
            {false, false, true, true, 0x7F10, 0x7F00, 0x8310, 0x8300, 0x1200, 0x200, 0x1A00, 0xA00, 0x40808080u},
            {false, false, true, true, 0x7F00, 0x7F10, 0x8300, 0x8310, 0x1200, 0x200, 0x1A00, 0xA00, 0x40808080u},
            {false, false, true, true, 0x7DF0, 0x73F0, 0x85F0, 0x7BF0, 0x1308, 0xD08, 0x1708, 0x1108, 0x40808080u}, // 64 -> 128
            {false, false, true, true, 0x7E10, 0x7400, 0x8610, 0x7C00, 0x1200, 0x200, 0x1A00, 0xA00, 0x40808080u},
            {false, false, true, true, 0x7E00, 0x7410, 0x8600, 0x7C10, 0x1200, 0x200, 0x1A00, 0xA00, 0x40808080u},
            // AE_ProcessSmartBlur: the 128x128 blur (rect 288,32, half-texel inset) over the frame, ALPHA 0x44
            {true, false, true, true, 0x6B60, 0x7160, 0x93F0, 0x8DF0, 0x1208, 0x208, 0x1A08, 0xA08, 0xFFFFFFFFu},
        };
        return b;
    }

    // CAfterEffect::AE_ProcessSmartBlur (prototype 0x466240) with AE_BlurBackBufferIntoZBuffer
    // (prototype 0x4666C0): the haze. `work` = the work buffer (FBP 280, shares memory with Z), at
    // least 448x288. Positions are the decompile's 12.4 values minus the XYOFFSET (1728, 1824); UVs
    // are 12.4 texels. `colour` = effect object +0x124 (RGB + strength in the top byte), `fade` =
    // +0x144 (0 when no fade runs). `steps` / `skipWork` run part of it (for the judge). Not
    // modelled: the half-width mode (flag 0x2000000 of the word at prototype 0x539254).
    // `scale` = internal resolution multiple (frame 640*scale wide): every position and UV scales
    // with it; `work` is sized here if empty.
    inline void smartBlur(Surface &frame, Surface &work, uint32_t colour, float fade = 0.0f, int steps = 11, bool skipWork = false,
                          int scale = 1)
    {
        scale = std::max(1, scale);
        if (work.rgba.empty())
        {
            work.w = 448 * scale;
            work.h = 288 * scale;
            work.rgba.assign(size_t(work.w) * work.h * 4, 0);
        }
        const float k = float(scale) / 16.0f;
        const int a = int(float(colour >> 24) * (1.0f - fade));
        const SmartBlurBlit *b = smartBlurBlits();
        for (int i = skipWork ? 10 : 0; i < steps && i < 11; ++i)
        {
            const SmartBlurBlit &s = b[i];
            const uint32_t col = s.colour == 0xFFFFFFFFu ? ((colour & 0xFFFFFFu) | (uint32_t(a & 0xFF) << 24)) : s.colour;
            gsSprite(s.toFrame ? frame : work, s.textured ? (s.fromFrame ? &frame : &work) : nullptr, (s.x0 - 0x6C00) * k,
                     (s.y0 - 0x7200) * k, (s.x1 - 0x6C00) * k, (s.y1 - 0x7200) * k, s.u0 * k, s.v0 * k, s.u1 * k, s.v1 * k, col,
                     s.blend ? Blend::Lerp44 : Blend::Copy);
        }
    }
}
