// pop_postfx_check: judges the CPU post-effect references in postfx.h against the per-pass
// framebuffer dumps of the running game (POP_GIF_TAP=1 POP_PASSDUMP_AT=<s>, game/recomp/screens).
//   pop_postfx_check smartblur <dir> <frame pass> <work pass> <first blur pass> [colour hex] [out.png]
// <frame pass> = the frame (FBP 140) just before the effect, <work pass> = the last dump of the work
// buffer (FBP 280) before it, <first blur pass> = the dump after the effect's first blit; the effect's
// ten work-buffer blits and the final blit onto the frame are compared with the dumps that follow.
// Without a colour the final blit's colour and strength are fitted to the dump.
#include "postfx.h"

#include <cstdio>
#include <cstdlib>
#include <string>

#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

namespace
{
    bool load(const std::string &dir, int pass, const char *fbp, pop_postfx::Surface &s)
    {
        char name[512];
        std::snprintf(name, sizeof(name), "%s/pass_%03d_%s.png", dir.c_str(), pass, fbp);
        int n = 0;
        unsigned char *p = stbi_load(name, &s.w, &s.h, &n, 4);
        if (!p)
        {
            std::fprintf(stderr, "cannot read %s\n", name);
            return false;
        }
        s.rgba.assign(p, p + size_t(s.w) * s.h * 4);
        stbi_image_free(p);
        // the dumps carry alpha in a separate *_alpha.png; the blits never write or read it (TCC 0)
        return true;
    }

    struct Rect
    {
        int x0, y0, x1, y1;
    };

    // mean and max absolute RGB difference inside r
    void compare(const char *what, const pop_postfx::Surface &a, const pop_postfx::Surface &b, Rect r)
    {
        double sum = 0;
        int mx = 0;
        size_t n = 0, off = 0;
        for (int y = r.y0; y < r.y1; ++y)
            for (int x = r.x0; x < r.x1; ++x)
                for (int c = 0; c < 3; ++c)
                {
                    const int d = std::abs(int(a.rgba[4 * (size_t(y) * a.w + x) + c]) - int(b.rgba[4 * (size_t(y) * b.w + x) + c]));
                    sum += d;
                    mx = std::max(mx, d);
                    off += d > 2;
                    ++n;
                }
        std::printf("%-34s mean %.3f  max %3d  >2: %.2f%%\n", what, sum / double(n), mx, 100.0 * double(off) / double(n));
    }
}

int main(int argc, char **argv)
{
    if (argc < 6 || std::string(argv[1]) != "smartblur")
    {
        std::fprintf(stderr, "usage: pop_postfx_check smartblur <dir> <frame pass> <work pass> <first blur pass> [colour hex] [out.png]\n");
        return 2;
    }
    const std::string dir = argv[2];
    const int framePass = std::atoi(argv[3]), workPass = std::atoi(argv[4]), first = std::atoi(argv[5]);
    pop_postfx::Surface frame, work;
    if (!load(dir, framePass, "fbp140", frame) || !load(dir, workPass, "fbp280", work))
        return 1;

    // Step through the ten work-buffer blits one at a time, each compared with its own dump
    // (written rectangles in work-buffer pixels).
    const Rect rects[10] = {{0, 0, 256, 256},     {256, 0, 448, 192},   {288, 32, 416, 160}, {288, 192, 384, 288},
                            {303, 207, 367, 271}, {305, 208, 369, 272}, {304, 209, 368, 273}, {287, 31, 415, 159},
                            {289, 32, 417, 160},  {288, 33, 416, 161}};
    // Run the whole effect on a copy for each prefix length; cheap enough (a few ms per run).
    for (int k = 1; k <= 10; ++k)
    {
        pop_postfx::Surface f = frame, w = work, ref;
        pop_postfx::smartBlur(f, w, 0, 0.0f, k);
        if (!load(dir, first + k - 1, "fbp280", ref))
            return 1;
        char what[64];
        std::snprintf(what, sizeof(what), "blit %2d (work buffer)", k);
        compare(what, w, ref, rects[k - 1]);
        if (std::getenv("POP_POSTFX_ERRMAP") && std::atoi(std::getenv("POP_POSTFX_ERRMAP")) == k)
        {
            const Rect r = rects[k - 1];
            for (int y = r.y0; y < r.y1; y += 8)
            {
                for (int x = r.x0; x < r.x1; x += 4)
                {
                    int e = 0;
                    for (int c = 0; c < 3; ++c)
                        e = std::max(e, std::abs(int(w.rgba[4 * (size_t(y) * w.w + x) + c]) - int(ref.rgba[4 * (size_t(y) * w.w + x) + c])));
                    std::putchar(e <= 2 ? '.' : e < 10 ? '1' : '#');
                }
                std::putchar('\n');
            }
        }
    }

    pop_postfx::Surface after;
    if (!load(dir, first + 10, "fbp140", after))
        return 1;
    uint32_t colour = 0;
    if (argc > 6)
        colour = uint32_t(std::strtoul(argv[6], nullptr, 16));
    else
    {
        // Fit: out = frame + (blur * k - frame) * a / 128 per channel. For each strength a solve k
        // per channel by least squares, keep the (a, k) with the smallest error of the exact blit.
        pop_postfx::Surface w = work, f = frame;
        pop_postfx::smartBlur(f, w, 0, 0.0f, 10);
        pop_postfx::Surface blurred = frame; // the final blit at full strength and neutral colour = the upsampled blur
        pop_postfx::smartBlur(blurred, w, 0x80808080u, 0.0f, 11, /*skipWork*/ true); // strength 0x80 = 1.0: the blur alone
        double best = 1e30;
        for (int a = 1; a <= 128; ++a)
        {
            uint32_t col = uint32_t(a) << 24;
            for (int c = 0; c < 3; ++c)
            {
                double num = 0, den = 0;
                for (size_t i = 0; i < size_t(frame.w) * frame.h; ++i)
                {
                    const double fr = frame.rgba[4 * i + c], out = after.rgba[4 * i + c], b = blurred.rgba[4 * i + c];
                    const double y = (out - fr) * 128.0 / a + fr; // = b * k / 128 * 128 / 128
                    num += b * y;
                    den += b * b;
                }
                const int k = std::clamp(int(std::lround(num / std::max(den, 1.0) * 128.0)), 0, 255);
                col |= uint32_t(k) << (8 * c);
            }
            pop_postfx::Surface t = frame, tw = w;
            pop_postfx::smartBlur(t, tw, col, 0.0f, 11, true);
            double err = 0;
            for (size_t i = 0; i < t.rgba.size(); ++i)
                if (i % 4 != 3)
                    err += std::abs(int(t.rgba[i]) - int(after.rgba[i]));
            if (err < best)
            {
                best = err;
                colour = col;
            }
        }
        std::printf("fitted colour (effect +0x124) = %08X\n", colour);
    }
    pop_postfx::Surface f = frame, w = work;
    pop_postfx::smartBlur(f, w, colour, 0.0f);
    {
        // the final blit alone, from the game's own work buffer (separates its error from the chain's)
        pop_postfx::Surface fr = frame, wr;
        if (load(dir, first + 9, "fbp280", wr))
        {
            pop_postfx::smartBlur(fr, wr, colour, 0.0f, 11, true);
            compare("final blit from the game's blur", fr, after, {0, 0, frame.w, frame.h});
        }
    }
    compare("final (frame)", f, after, {0, 0, frame.w, frame.h});
    compare("frame before vs after (no haze)", frame, after, {0, 0, frame.w, frame.h});
    if (argc > 7)
    {
        std::vector<uint8_t> out = f.rgba;
        for (size_t i = 3; i < out.size(); i += 4)
            out[i] = 255;
        stbi_write_png(argv[7], f.w, f.h, 4, out.data(), f.w * 4);
    }
    return 0;
}
