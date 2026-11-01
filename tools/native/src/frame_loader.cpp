// Native frames into a Renderer (see frame_loader.h). Format history: PNF1 = clip + draws (model,
// batches); PNF2 + images and per draw alpha mode + cutoff; PNF3 render state flags + AREF word
// instead; PNF4 + fog a, b, FOGCOL, Z test per draw (game order); PNF5 + FBMSK per draw, GLOW and
// HAZE trailers; PNF6 = PNF5 with images by texture key, pixels only the first time; PNF7 (the in-game
// native world) = PNF6 with a 64-bit key per batch, its geometry only the first time (kept meshes); key 0 =
// a batch for this frame only (posed characters), always with its geometry. PNF8 = "PNF8", the layout it
// follows (5, 6 or 7), then that layout with fog min, max after fog b per draw (the VU1 drivers clamp F to them).
#include "frame_loader.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <tuple>

namespace pop_native
{
    bool frameInGameOrder(const char magic[4])
    {
        return std::memcmp(magic, "PNF4", 4) == 0 || std::memcmp(magic, "PNF5", 4) == 0 || std::memcmp(magic, "PNF6", 4) == 0 ||
               std::memcmp(magic, "PNF7", 4) == 0 || std::memcmp(magic, "PNF8", 4) == 0;
    }

    PostEffects postEffectsOf(const FrameExtras &e)
    {
        PostEffects pe;
        pe.glow = e.glowOn;
        pe.levels = int(e.glow[0]);
        pe.fix = int(e.glow[1]);
        pe.mask = e.glow[2] != 0;
        pe.convolution = e.glow[3] != 0;
        pe.strength = int(e.glow[4]);
        pe.haze = e.hazeOn;
        pe.hazeColour = e.haze[0];
        std::memcpy(&pe.hazeFade, &e.haze[1], 4);
        pe.motionSmooth = e.after[0] & 1;
        pe.motionDx = int(e.after[1]);
        pe.motionDy = int(e.after[2]);
        pe.fixBlend = e.after[0] & 2 ? int(e.after[3]) : 0;
        pe.zoomSmooth = e.zoom;
        return pe;
    }

    bool FrameLoader::load(Renderer &r, std::istream &f, const char *name, float clip[16], std::vector<Draw> &draws, FrameExtras &extras,
                           bool verbose)
    {
        char magic[4] = {};
        f.read(magic, 4);
        // PNF8: a word naming the layout (5, 6 or 7) follows, then that layout + fog min, max per draw
        const bool v8 = std::memcmp(magic, "PNF8", 4) == 0;
        uint32_t layout = 0;
        if (v8)
            f.read(reinterpret_cast<char *>(&layout), 4);
        const bool v7 = layout == 7 || std::memcmp(magic, "PNF7", 4) == 0; // + batch keys, geometry only the first time
        const bool v6 = v7 || layout == 6 || std::memcmp(magic, "PNF6", 4) == 0; // live: images by texture key, pixels only the first time
        const bool v5 = v6 || layout == 5 || std::memcmp(magic, "PNF5", 4) == 0; // + FBMSK per draw, GLOW trailer
        const bool v4 = v5 || std::memcmp(magic, "PNF4", 4) == 0; // + fog a, b, FOGCOL, Z test per draw
        const bool v3 = v4 || std::memcmp(magic, "PNF3", 4) == 0;
        const bool v2 = v3 || std::memcmp(magic, "PNF2", 4) == 0;
        if (!v2 && std::memcmp(magic, "PNF1", 4) != 0)
            return false;
        f.read(reinterpret_cast<char *>(clip), 64);
        std::vector<int> imageIndex;
        if (v2)
        {
            uint32_t ni = 0;
            f.read(reinterpret_cast<char *>(&ni), 4);
            for (uint32_t i = 0; i < ni && f; ++i)
            {
                Image img;
                uint32_t key = 0;
                if (v6)
                    f.read(reinterpret_cast<char *>(&key), 4);
                f.read(reinterpret_cast<char *>(&img.width), 4);
                f.read(reinterpret_cast<char *>(&img.height), 4);
                if (v6 && img.width == 0)
                {
                    const auto it = imageOf_.find(key); // sent in an earlier frame
                    imageIndex.push_back(it != imageOf_.end() ? it->second : -1);
                    continue;
                }
                img.rgba.resize(size_t(img.width) * img.height * 4);
                f.read(reinterpret_cast<char *>(img.rgba.data()), std::streamsize(img.rgba.size()));
                // a key sent again with pixels (shadow textures, every frame): the same image, new pixels
                if (v6)
                    if (const auto it = imageOf_.find(key); it != imageOf_.end() && r.updateImage(it->second, img))
                    {
                        imageIndex.push_back(it->second);
                        continue;
                    }
                imageIndex.push_back(r.addImage(img));
                if (v6)
                    imageOf_[key] = imageIndex.back();
                ++imageTotal;
            }
        }
        std::map<std::tuple<int, uint32_t, uint32_t, uint32_t>, int> materials;
        uint32_t n = 0;
        f.read(reinterpret_cast<char *>(&n), 4);
        for (uint32_t i = 0; i < n && f; ++i)
        {
            Draw d;
            f.read(reinterpret_cast<char *>(d.model), 64);
            int material = -1;
            bool haloCopy = false;
            if (v2)
            {
                int32_t image = -1;
                uint32_t mode = 0, word = 0; // PNF2: alpha mode, cutoff (float bits); PNF3: flags, AREF word
                f.read(reinterpret_cast<char *>(&image), 4);
                f.read(reinterpret_cast<char *>(&mode), 4);
                f.read(reinterpret_cast<char *>(&word), 4);
                uint32_t fogColour = 0, zTest = 1, fbmsk = 0xFF000000u;
                if (v4)
                {
                    f.read(reinterpret_cast<char *>(&d.fogA), 4);
                    f.read(reinterpret_cast<char *>(&d.fogB), 4);
                    if (v8)
                    {
                        f.read(reinterpret_cast<char *>(&d.fogMin), 4);
                        f.read(reinterpret_cast<char *>(&d.fogMax), 4);
                    }
                    f.read(reinterpret_cast<char *>(&fogColour), 4);
                    f.read(reinterpret_cast<char *>(&zTest), 4);
                    if (v5)
                        f.read(reinterpret_cast<char *>(&fbmsk), 4);
                    d.ownFog = d.ownFogColour = true;
                    // Z test word bits 2 / 3 (0.14, ours): the 2D layer (after the post effects); model = screen pixels -> clip
                    d.afterPost = (zTest & 4) != 0;
                    d.screen = (zTest & 8) != 0;
                    for (int k = 0; k < 3; ++k)
                        d.fogColour[k] = float((fogColour >> (8 * k)) & 0xFF) / 255.0f;
                }
                const bool hasImage = image >= 0 && size_t(image) < imageIndex.size();
                // image -2: the water's colour pass (pop_native_live waterRecord): the frame is copied before the draw and
                // read through the refraction buffer
                const bool refraction = v3 && image == -2;
                // image -3: a halo's visibility (pop_native_live haloRecord): the frame is copied before the draw, its alpha averaged;
                // -6: the halo's square saved before it touches the alpha (draws nothing); -4: that save read back (the glow
                // mask put back after the halo)
                const bool haloAlpha = v3 && (image == -3 || image == -4);
                haloCopy = v3 && (image == -3 || image == -6);
                d.saveFrame = v3 && image == -6;
                // -5: the pinch effect's grid (pop_native_live pinchRecord): the frame is copied before the draw and read as it is
                const bool pinch = v3 && image == -5;
                d.copyFrame = refraction || (haloCopy && !d.saveFrame) || pinch;
                if (hasImage || v3)
                {
                    const auto key = std::make_tuple(hasImage || refraction || haloAlpha || pinch ? image : -1, mode, word,
                                                     (zTest & 3u) | (fbmsk & 0xFF000000u) | ((fbmsk & 0xFFFFFF) ? 4u : 0u));
                    if (!materials.count(key))
                    {
                        Material mat;
                        mat.baseImage = hasImage                          ? imageIndex[size_t(image)]
                                        : v3 && image == -4               ? kFrameSaveImage
                                        : refraction || haloAlpha || pinch ? kFrameCopyImage
                                                                           : -1;
                        mat.refraction = refraction;
                        mat.haloAlpha = haloAlpha;
                        if (v3)
                        {
                            materialFromGsFlags(mode, word, mat);
                            static const bool noTest = std::getenv("POP_VIEW_NOTEST") != nullptr; // debug: alpha test off
                            if (noTest)
                                mat.alphaTest = false;
                            // Z test word: bit 0 = the render state's Z test (0x70000038); bit 1 = ours, the
                            // texture clamped to its edge (shadow receivers, GS CLAMP_1 5); bits 2, 3: Draw::afterPost, screen
                            mat.depthTest = (zTest & 1) != 0;
                            mat.clampUv = (zTest & 2) != 0;
                            mat.writeRgb = (fbmsk & 0xFFFFFFu) != 0xFFFFFFu;
                            mat.writeAlpha = (fbmsk & 0xFF000000u) == 0;
                        }
                        else
                        {
                            mat.alphaMode = static_cast<AlphaMode>(mode);
                            std::memcpy(&mat.cutoff, &word, 4);
                            mat.legacyBlend = mat.alphaMode == AlphaMode::Blend;
                        }
                        materials[key] = r.addMaterial(mat);
                        if (std::getenv("POP_VIEW_LIST"))
                            std::printf("material %d: image %d, flags %08x, word %08x, z %u, fbmsk %08x\n", materials[key], image,
                                        mode, word, zTest, fbmsk);
                    }
                    material = materials[key];
                }
            }
            uint32_t nb = 0;
            f.read(reinterpret_cast<char *>(&nb), 4);
            for (uint32_t b = 0; b < nb && f; ++b)
            {
                uint32_t nv = 0, ni = 0;
                uint64_t key = 0;
                if (v7)
                    f.read(reinterpret_cast<char *>(&key), 8);
                f.read(reinterpret_cast<char *>(&nv), 4);
                if (v7 && nv == 0xFFFFFFFFu) // sent before: the renderer keeps it
                {
                    d.mesh = -1;
                    d.kept = r.keptMesh(key);
                    d.material = material;
                    if (d.kept < 0)
                        ++keptMissing;
                    else
                        draws.push_back(d), d.copyFrame = d.saveFrame = false;
                    continue;
                }
                f.read(reinterpret_cast<char *>(&ni), 4);
                Mesh mesh;
                mesh.vertices.resize(nv);
                mesh.indices.resize(ni);
                f.read(reinterpret_cast<char *>(mesh.vertices.data()), std::streamsize(nv) * sizeof(Vertex));
                f.read(reinterpret_cast<char *>(mesh.indices.data()), std::streamsize(ni) * 4);
                // a halo's spread and restore read the frame copy only inside the spread's own square: copy just that, with
                // two GS pixels to spare (frame UV = (NDC + 1) / 2; columns of the column-major model and clip matrices)
                if (haloCopy && (d.copyFrame || d.saveFrame) && !mesh.vertices.empty())
                {
                    float box[4] = {1, 1, 0, 0};
                    for (const Vertex &v : mesh.vertices)
                    {
                        float p[4], c[4];
                        for (int r = 0; r < 4; ++r)
                            p[r] = d.model[r] * v.pos[0] + d.model[4 + r] * v.pos[1] + d.model[8 + r] * v.pos[2] + d.model[12 + r];
                        for (int r = 0; r < 4; ++r)
                            c[r] = clip[r] * p[0] + clip[4 + r] * p[1] + clip[8 + r] * p[2] + clip[12 + r] * p[3];
                        if (!(c[3] > 0.0f))
                            box[0] = box[1] = 0, box[2] = box[3] = 1; // behind the eye: copy it all
                        const float u = (c[0] / c[3] + 1) / 2, w = (c[1] / c[3] + 1) / 2;
                        box[0] = std::min(box[0], u), box[1] = std::min(box[1], w), box[2] = std::max(box[2], u), box[3] = std::max(box[3], w);
                    }
                    d.copyRect[0] = box[0] - 2 / 640.0f, d.copyRect[1] = box[1] - 2 / 448.0f;
                    d.copyRect[2] = box[2] + 2 / 640.0f, d.copyRect[3] = box[3] + 2 / 448.0f;
                }
                // no robustBufferAccess: an index past the vertices reads arbitrary GPU memory, so drop the batch
                if (f)
                    if (const auto bad = std::find_if(mesh.indices.begin(), mesh.indices.end(), [nv](uint32_t x) { return x >= nv; });
                        bad != mesh.indices.end())
                    {
                        std::fprintf(stderr, "frame %s: draw %u batch %u index %u >= %u vertices, batch dropped\n", name, i, b, *bad, nv);
                        continue;
                    }
                mesh.material = material;
                // debug: POP_VIEW_VERTS=<draw> prints that draw's vertices (position, UV, colour x 0x80)
                static const char *verts = std::getenv("POP_VIEW_VERTS");
                if (verts && std::atoi(verts) == int(i))
                {
                    for (size_t k = 0; k + 2 < mesh.indices.size(); k += 3)
                        std::printf("draw %u batch %u: triangle %u %u %u\n", i, b, mesh.indices[k], mesh.indices[k + 1], mesh.indices[k + 2]);
                    for (const Vertex &v : mesh.vertices)
                        std::printf("draw %u batch %u: pos %.2f %.2f %.2f uv %.3f %.3f rgba %.1f %.1f %.1f %.1f\n", i, b, double(v.pos[0]),
                                    double(v.pos[1]), double(v.pos[2]), double(v.uv[0]), double(v.uv[1]), double(v.colour[0] * 128),
                                    double(v.colour[1] * 128), double(v.colour[2] * 128), double(v.colour[3] * 128));
                }
                vertexTotal += nv;
                triTotal += ni / 3;
                if (v7 && key) // key 0: this frame only (posed characters), an ordinary mesh
                {
                    d.mesh = -1;
                    d.kept = r.keepMesh(key, std::move(mesh));
                    d.material = material;
                    draws.push_back(d);
                    d.copyFrame = d.saveFrame = false;
                    continue;
                }
                d.mesh = r.addMesh(std::move(mesh));
                // debug: POP_VIEW_ONLY=<first>-<last> keeps only those element draws (bisecting a frame)
                static const char *only = std::getenv("POP_VIEW_ONLY");
                if (!only || (int(i) >= std::atoi(only) && int(i) <= std::atoi(std::strchr(only, '-') ? std::strchr(only, '-') + 1 : only)))
                    draws.push_back(d), d.copyFrame = d.saveFrame = false;
            }
            static const bool listDraws = std::getenv("POP_VIEW_LIST") != nullptr;
            if (listDraws)
                std::printf("draw %u: %u batches, material %d, model t (%.1f %.1f %.1f), fog %.2f %+.4f w [%.0f %.0f] colour %.0f %.0f %.0f\n", i,
                            nb, material, double(d.model[12]), double(d.model[13]), double(d.model[14]), double(d.fogA), double(d.fogB),
                            double(d.fogMin), double(d.fogMax), double(d.fogColour[0] * 255), double(d.fogColour[1] * 255),
                            double(d.fogColour[2] * 255));
        }
        if (v5)
        {
            char tag[4] = {};
            uint32_t calls = 0;
            if (f.read(tag, 4) && std::memcmp(tag, "GLOW", 4) == 0 && f.read(reinterpret_cast<char *>(&calls), 4))
            {
                f.read(reinterpret_cast<char *>(extras.glow), sizeof extras.glow);
                extras.glowOn = calls > 0;
                if (verbose)
                    std::printf("glow (ColorDiffusion): %u calls, levels %u FIX %u mask %u convolution %u strength %d\n", calls, extras.glow[0],
                                extras.glow[1], extras.glow[2], extras.glow[3], int(extras.glow[4]));
            }
            if (f.read(tag, 4) && std::memcmp(tag, "HAZE", 4) == 0 && f.read(reinterpret_cast<char *>(&calls), 4))
            {
                f.read(reinterpret_cast<char *>(extras.haze), sizeof extras.haze);
                extras.hazeOn = calls > 0;
                float fade = 0;
                std::memcpy(&fade, &extras.haze[1], 4);
                if (verbose)
                    std::printf("haze (SmartBlur): %u calls, colour %08X fade %.3f\n", calls, extras.haze[0], double(fade));
            }
            std::memset(extras.after, 0, sizeof extras.after);
            bool more = bool(f.read(tag, 4));
            if (more && std::memcmp(tag, "AFTR", 4) == 0) // MotionSmooth / FixBlend
            {
                f.read(reinterpret_cast<char *>(extras.after), sizeof extras.after);
                if (verbose && extras.after[0])
                    std::printf("after effects: MotionSmooth %u (dx %d dy %d), FixBlend %u (FIX %u)\n", extras.after[0] & 1,
                                int(extras.after[1]), int(extras.after[2]), (extras.after[0] >> 1) & 1, extras.after[3]);
                more = bool(f.read(tag, 4));
            }
            extras.zoom = 0.0f;
            if (more && std::memcmp(tag, "ZOOM", 4) == 0) // ZoomSmooth 0
            {
                f.read(reinterpret_cast<char *>(&extras.zoom), 4);
                if (verbose && extras.zoom > 0.0f)
                    std::printf("after effects: ZoomSmooth 0 strength %.3f\n", double(extras.zoom));
                more = bool(f.read(tag, 4));
            }
            // live frames: the game's own GS pictures latched at the next frame boundaries
            // (POP_NATIVE_WINDOW_GS=<d>); the first is shown with T, all are scored on --live-shots
            extras.gsImages.clear();
            uint32_t gw = 0, gh = 0;
            for (; more && std::memcmp(tag, "GSFB", 4) == 0 && f.read(reinterpret_cast<char *>(&gw), 4) &&
                   f.read(reinterpret_cast<char *>(&gh), 4) && gw && gh && gw <= 4096 && gh <= 4096;
                 more = bool(f.read(tag, 4)))
            {
                Image g;
                g.width = gw;
                g.height = gh;
                g.rgba.resize(size_t(gw) * gh * 4);
                f.read(reinterpret_cast<char *>(g.rgba.data()), std::streamsize(g.rgba.size()));
                extras.gsImages.push_back(std::move(g));
            }
        }
        if (verbose)
            std::printf("frame %s: %u element draws, %zu materials\n", name, n, materials.size());
        return bool(f) || f.eof();
    }
}
