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
}
