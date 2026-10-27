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
                    for (int k = 0; k < 3; ++k)
                        d.fogColour[k] = float((fogColour >> (8 * k)) & 0xFF) / 255.0f;
                }
                const bool hasImage = image >= 0 && size_t(image) < imageIndex.size();
                if (hasImage || v3)
                {
                    const auto key = std::make_tuple(hasImage ? image : -1, mode, word, zTest | (fbmsk & 0xFF000000u) | ((fbmsk & 0xFFFFFF) ? 4u : 0u));
                    if (!materials.count(key))
                    {
                        Material mat;
                        mat.baseImage = hasImage ? imageIndex[size_t(image)] : -1;
                        if (v3)
                        {
                            materialFromGsFlags(mode, word, mat);
                            static const bool noTest = std::getenv("POP_VIEW_NOTEST") != nullptr; // debug: alpha test off
                            if (noTest)
                                mat.alphaTest = false;
                            // Z test word: bit 0 = the render state's Z test (0x70000038); bit 1 = ours, the
                            // texture clamped to its edge (shadow receivers, GS CLAMP_1 5)
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
                        draws.push_back(d);
                    continue;
                }
                f.read(reinterpret_cast<char *>(&ni), 4);
                Mesh mesh;
                mesh.vertices.resize(nv);
                mesh.indices.resize(ni);
                f.read(reinterpret_cast<char *>(mesh.vertices.data()), std::streamsize(nv) * sizeof(Vertex));
                f.read(reinterpret_cast<char *>(mesh.indices.data()), std::streamsize(ni) * 4);
                // no robustBufferAccess: an index past the vertices reads arbitrary GPU memory, so drop the batch
                if (f)
                    if (const auto bad = std::find_if(mesh.indices.begin(), mesh.indices.end(), [nv](uint32_t x) { return x >= nv; });
                        bad != mesh.indices.end())
                    {
                        std::fprintf(stderr, "frame %s: draw %u batch %u index %u >= %u vertices, batch dropped\n", name, i, b, *bad, nv);
                        continue;
                    }
                mesh.material = material;
                vertexTotal += nv;
                triTotal += ni / 3;
                if (v7 && key) // key 0: this frame only (posed characters), an ordinary mesh
                {
                    d.mesh = -1;
                    d.kept = r.keepMesh(key, std::move(mesh));
                    d.material = material;
                    draws.push_back(d);
                    continue;
                }
                d.mesh = r.addMesh(std::move(mesh));
                // debug: POP_VIEW_ONLY=<first>-<last> keeps only those element draws (bisecting a frame)
                static const char *only = std::getenv("POP_VIEW_ONLY");
                if (!only || (int(i) >= std::atoi(only) && int(i) <= std::atoi(std::strchr(only, '-') ? std::strchr(only, '-') + 1 : only)))
                    draws.push_back(d);
            }
            static const bool listDraws = std::getenv("POP_VIEW_LIST") != nullptr;
            if (listDraws)
                std::printf("draw %u: %u batches, material %d, model t (%.1f %.1f %.1f)\n", i, nb, material, double(d.model[12]),
                            double(d.model[13]), double(d.model[14]));
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
            // live frames: the game's own GS pictures latched at the next frame boundaries
            // (POP_NATIVE_WINDOW_GS=<d>); the first is shown with T, all are scored on --live-shots
            extras.gsImages.clear();
            uint32_t gw = 0, gh = 0;
            while (f.read(tag, 4) && std::memcmp(tag, "GSFB", 4) == 0 && f.read(reinterpret_cast<char *>(&gw), 4) &&
                   f.read(reinterpret_cast<char *>(&gh), 4) && gw && gh && gw <= 4096 && gh <= 4096)
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
