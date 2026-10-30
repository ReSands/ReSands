// pop_native_view: renders a level converted by tools/jade2gltf.py (--place) with the native Vulkan
// renderer through the game's own camera, to a PNG that tools/imgscore.py can compare with a GS
// frame of the same moment.
//
// Usage: pop_native_view <scene.gltf> <clip file> <out.png> [--nearest] [--width W --height H]
//                        [--depth <file>] (raw float32 view depth per pixel) [--gsz <file>] (raw uint32 GS Z24)
//                        [--add <more.gltf>]...
//                        [--fog a,b,R,G,B] (GS fog: F = a + b * depth, FOGCOL) [--addfog a,b <gltf>] (own fog)
//        pop_native_view <scene.gltf> <clip file> <screenshot.png> --window [--scale N]
//          interactive: starts at the game's camera. Mouse drag (left button) looks around, WASD
//          moves, Q/E down/up, Shift faster, R back to the game camera, 1-4 internal resolution
//          (640x448 times N), P saves a screenshot at the internal resolution, Esc quits.
//          --frames N quits after N frames (for tests).
//   clip file: the game's world -> clip matrix, 16 comma- or space-separated floats (row vectors, as
//   graphics system +0x240 holds it; POP_CAMLOG / POP_FRAMECAP "capture" lines carry it).
#include "assets.h"
#include "frame_loader.h"
#include "free_camera.h"
#include "native_renderer.h"
#include "postfx.h"
#include "live_socket.h"

#include <atomic>
#include <mutex>
#include <sstream>
#include <thread>
#include <unordered_map>

#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>

#define CGLTF_IMPLEMENTATION
#include <cgltf.h>
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#include <stb_image.h>
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image_write.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <iterator>
#include <tuple>
#include <vector>

using namespace pop_native;

namespace
{
    bool loadImage(const cgltf_image *img, const cgltf_options &opt, Image &out)
    {
        std::vector<uint8_t> bytes;
        if (img->buffer_view)
        {
            const auto *bv = img->buffer_view;
            const uint8_t *p = static_cast<const uint8_t *>(bv->buffer->data) + bv->offset;
            bytes.assign(p, p + bv->size);
        }
        else if (img->uri && std::strncmp(img->uri, "data:", 5) == 0)
        {
            const char *comma = std::strchr(img->uri, ',');
            if (!comma)
                return false;
            const char *b64 = comma + 1;
            const size_t len = std::strlen(b64);
            size_t size = len / 4 * 3;
            if (len >= 1 && b64[len - 1] == '=')
                --size;
            if (len >= 2 && b64[len - 2] == '=')
                --size;
            void *data = nullptr;
            if (cgltf_load_buffer_base64(&opt, size, b64, &data) != cgltf_result_success)
                return false;
            bytes.assign(static_cast<uint8_t *>(data), static_cast<uint8_t *>(data) + size);
            std::free(data);
        }
        else
            return false;
        int w, h, n;
        stbi_uc *px = stbi_load_from_memory(bytes.data(), static_cast<int>(bytes.size()), &w, &h, &n, 4);
        if (!px)
            return false;
        out.width = static_cast<uint32_t>(w);
        out.height = static_cast<uint32_t>(h);
        out.rgba.assign(px, px + size_t(w) * h * 4);
        stbi_image_free(px);
        return true;
    }

    bool readClip(const char *path, float clip[16])
    {
        std::ifstream f(path);
        std::stringstream ss;
        ss << f.rdbuf();
        std::string s = ss.str();
        for (char &c : s)
            if (c == ',')
                c = ' ';
        std::istringstream in(s);
        for (int i = 0; i < 16; ++i)
            if (!(in >> clip[i]))
                return false;
        return true;
    }
}

int main(int argc, char **argv)
{
    if (argc < 4)
    {
        std::fprintf(stderr, "usage: %s <scene.gltf> <clip file> <out.png> [--nearest] [--width W --height H]\n"
                             "       %s <scene.txt> <clip file> <out.png> --pack <unpacked pack.bin> (C++ asset conversion; scene\n"
                             "       from tools/jade2gltf.py --place --static-only --scene)\n", argv[0], argv[0]);
        return 2;
    }
    Options options;
    uint32_t width = 640, height = 448;
    bool windowed = false;
    uint32_t scale = 1;
    long maxFrames = -1; // --frames N: quit after N presented frames (tests)
    int livePort = 0;    // --live PORT: the live native window, fed frames by the running game (POP_NATIVE_WINDOW)
    const char *presentShot = nullptr; // --present-shot <png>: with --frames, save the last presented frame
    double liveShotSeconds = 0; // --live-shots S: save the live frame (game camera) every S seconds as <out>_live_<n>.png
    for (int i = 4; i < argc; ++i)
    {
        if (!std::strcmp(argv[i], "--nearest"))
            options.linearFilter = false;
        else if (!std::strcmp(argv[i], "--cull") && i + 1 < argc)
            options.cull = std::atoi(argv[++i]);
        else if (!std::strcmp(argv[i], "--cull-ccw"))
            options.cullClockwise = false;
        else if (!std::strcmp(argv[i], "--window"))
            windowed = true;
        else if (!std::strcmp(argv[i], "--present-shot") && i + 1 < argc)
            presentShot = argv[++i];
        else if (!std::strcmp(argv[i], "--live-shots") && i + 1 < argc)
            liveShotSeconds = std::atof(argv[++i]);
        else if (!std::strcmp(argv[i], "--live") && i + 1 < argc)
        {
            livePort = std::atoi(argv[++i]);
            windowed = true;
        }
        else if (!std::strcmp(argv[i], "--frames") && i + 1 < argc)
            maxFrames = std::atol(argv[++i]);
        else if (!std::strcmp(argv[i], "--scale") && i + 1 < argc)
            scale = static_cast<uint32_t>(std::max(1, std::atoi(argv[++i])));
        else if (!std::strcmp(argv[i], "--width") && i + 1 < argc)
            width = static_cast<uint32_t>(std::atoi(argv[++i]));
        else if (!std::strcmp(argv[i], "--height") && i + 1 < argc)
            height = static_cast<uint32_t>(std::atoi(argv[++i]));
    }
    float clip[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    if (!livePort && !readClip(argv[2], clip))
    {
        std::fprintf(stderr, "cannot read 16 floats from %s\n", argv[2]);
        return 1;
    }
    const auto t0 = std::chrono::steady_clock::now();
    const char *packPath = nullptr;
    bool framePath = false; // --frame: argv[1] is a frame exported by the runtime (POP_NATIVE_FRAME)
    for (int i = 4; i < argc; ++i)
        if (!std::strcmp(argv[i], "--pack") && i + 1 < argc)
            packPath = argv[i + 1];
        else if (!std::strcmp(argv[i], "--frame"))
            framePath = true;
    if (livePort)
        options.gameOrder = true; // live frames are PNF5
    else if (framePath)
    {
        // PNF4 onwards list the draws in the game's own order: draw them in one pass in that order (as the
        // game window does; two passes put translucents over later opaque draws, e.g. the sky's cloud layer
        // over the horizon band drawn after it, save 21_202624)
        std::ifstream peek(argv[1], std::ios::binary);
        char magic[4] = {};
        peek.read(magic, 4);
        options.gameOrder = std::memcmp(magic, "PNF", 3) == 0 && magic[3] >= '4' && magic[3] <= '9';
    }
    cgltf_options opt{};
    cgltf_data *data = nullptr;
    if (!packPath && !framePath && !livePort && (cgltf_parse_file(&opt, argv[1], &data) != cgltf_result_success || cgltf_load_buffers(&opt, data, argv[1]) != cgltf_result_success))
    {
        std::fprintf(stderr, "cannot load %s\n", argv[1]);
        return 1;
    }

    SDL_Window *window = nullptr;
    if (windowed)
    {
        if (!SDL_Init(SDL_INIT_VIDEO))
        {
            std::fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
            return 1;
        }
#if defined(__APPLE__)
        // Homebrew's vulkan-loader is not on the default dyld search path
        if (!SDL_Vulkan_LoadLibrary(nullptr))
            SDL_Vulkan_LoadLibrary("/opt/homebrew/lib/libvulkan.1.dylib");
#endif
        window = SDL_CreateWindow("PoP native view", 1280, 896, SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY);
        if (!window)
        {
            std::fprintf(stderr, "SDL_CreateWindow: %s\n", SDL_GetError());
            return 1;
        }
        Uint32 n = 0;
        const char *const *exts = SDL_Vulkan_GetInstanceExtensions(&n);
        for (Uint32 i = 0; i < n; ++i)
            options.instanceExtensions.emplace_back(exts[i]);
        options.createSurface = [window](void *instance) -> uint64_t {
            VkSurfaceKHR surface = 0;
            if (!SDL_Vulkan_CreateSurface(window, static_cast<VkInstance>(instance), nullptr, &surface))
                std::fprintf(stderr, "SDL_Vulkan_CreateSurface: %s\n", SDL_GetError());
            return (uint64_t)surface;
        };
    }
    Renderer r;
    std::string error;
    if (!r.init(options, error))
    {
        std::fprintf(stderr, "Vulkan: %s\n", error.c_str());
        return 1;
    }

    // one renderer mesh per glTF primitive, one draw per (node, primitive); every scene file adds to them
    std::vector<Draw> draws;
    size_t vertexTotal = 0, triTotal = 0, imageTotal = 0;
    auto addScene = [&](cgltf_data *data, const float *ownFog) {
        imageTotal += data->images_count;
        std::vector<int> imageIndex(data->images_count, -1);
        for (size_t i = 0; i < data->images_count; ++i)
        {
            Image img;
            if (loadImage(&data->images[i], opt, img))
                imageIndex[i] = r.addImage(img);
        }
        auto texImage = [&](const cgltf_texture_view &v) {
            return v.texture && v.texture->image ? imageIndex[cgltf_image_index(data, v.texture->image)] : -1;
        };
        std::vector<int> materialIndex(data->materials_count, -1);
        for (size_t i = 0; i < data->materials_count; ++i)
        {
            const cgltf_material &gm = data->materials[i];
            Material mat;
            mat.baseImage = texImage(gm.pbr_metallic_roughness.base_color_texture);
            mat.lightmapImage = texImage(gm.occlusion_texture); // jade2gltf: lightmap page on TEXCOORD_1
            if (mat.lightmapImage >= 0 && gm.occlusion_texture.scale > 0.0f)
                mat.lightmapStrength = gm.occlusion_texture.scale; // glTF occlusion strength (default 1)
            mat.alphaMode = gm.alpha_mode == cgltf_alpha_mode_mask    ? AlphaMode::Mask
                            : gm.alpha_mode == cgltf_alpha_mode_blend ? AlphaMode::Blend
                                                                      : AlphaMode::Opaque;
            mat.cutoff = gm.alpha_cutoff;
            mat.legacyBlend = mat.alphaMode == AlphaMode::Blend;
            mat.doubleSided = gm.double_sided;
            materialIndex[i] = r.addMaterial(mat);
        }

        // one renderer mesh per glTF primitive
        std::map<const cgltf_mesh *, std::vector<int>> meshPrims;
        for (size_t mi = 0; mi < data->meshes_count; ++mi)
        {
            const cgltf_mesh &gm = data->meshes[mi];
            for (size_t pi = 0; pi < gm.primitives_count; ++pi)
            {
                const cgltf_primitive &p = gm.primitives[pi];
                const cgltf_accessor *pos = nullptr, *uv = nullptr, *uv2 = nullptr, *col = nullptr;
                for (size_t a = 0; a < p.attributes_count; ++a)
                {
                    const cgltf_attribute &at = p.attributes[a];
                    if (at.type == cgltf_attribute_type_position)
                        pos = at.data;
                    else if (at.type == cgltf_attribute_type_texcoord && at.index == 0)
                        uv = at.data;
                    else if (at.type == cgltf_attribute_type_texcoord && at.index == 1)
                        uv2 = at.data;
                    else if (at.type == cgltf_attribute_type_color && at.index == 0)
                        col = at.data;
                }
                if (!pos || p.type != cgltf_primitive_type_triangles)
                    continue;
                Mesh mesh;
                mesh.vertices.resize(pos->count);
                for (size_t v = 0; v < pos->count; ++v)
                {
                    Vertex &vx = mesh.vertices[v];
                    float c[4] = {1, 1, 1, 1};
                    cgltf_accessor_read_float(pos, v, vx.pos, 3);
                    vx.uv[0] = vx.uv[1] = vx.uv2[0] = vx.uv2[1] = 0;
                    if (uv)
                        cgltf_accessor_read_float(uv, v, vx.uv, 2);
                    if (uv2)
                        cgltf_accessor_read_float(uv2, v, vx.uv2, 2);
                    if (col)
                        cgltf_accessor_read_float(col, v, c, col->type == cgltf_type_vec3 ? 3 : 4);
                    std::memcpy(vx.colour, c, sizeof c);
                }
                if (p.indices)
                    for (size_t k = 0; k < p.indices->count; ++k)
                        mesh.indices.push_back(static_cast<uint32_t>(cgltf_accessor_read_index(p.indices, k)));
                else
                    for (size_t k = 0; k < pos->count; ++k)
                        mesh.indices.push_back(static_cast<uint32_t>(k));
                mesh.material = p.material ? materialIndex[cgltf_material_index(data, p.material)] : -1;
                // a material whose lightmap exists but the primitive has no second UV set: no lightmap
                vertexTotal += mesh.vertices.size();
                triTotal += mesh.indices.size() / 3;
                meshPrims[&gm].push_back(r.addMesh(std::move(mesh)));
            }
        }

        for (size_t ni = 0; ni < data->nodes_count; ++ni)
        {
            const cgltf_node &n = data->nodes[ni];
            if (!n.mesh)
                continue;
            Draw d;
            // object -> game world: the node's and its parents' local transforms, except jade2gltf's
            // root "world (Z-up)" (Z-up -> glTF Y-up), since the game's clip matrix works in Z-up
            float m[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
            for (const cgltf_node *p = &n; p; p = p->parent)
            {
                if (p->name && std::strncmp(p->name, "world (Z-up)", 12) == 0)
                    continue;
                float local[16], out[16];
                cgltf_node_transform_local(p, local);
                for (int c = 0; c < 4; ++c) // out = local * m
                    for (int r = 0; r < 4; ++r)
                    {
                        float s = 0;
                        for (int k = 0; k < 4; ++k)
                            s += local[k * 4 + r] * m[c * 4 + k];
                        out[c * 4 + r] = s;
                    }
                std::memcpy(m, out, sizeof m);
            }
            std::memcpy(d.model, m, sizeof m);
            if (ownFog)
            {
                d.ownFog = true;
                d.fogA = ownFog[0];
                d.fogB = ownFog[1];
            }
            for (int m : meshPrims[n.mesh])
            {
                d.mesh = m;
                draws.push_back(d);
            }
        }
    };
    // --pack: the scene converted by pop_assets (C++) from the pack, placed as jade2gltf decided
    // (scene file: E key offset matid entry chain / O name geo mm lightmap 16 floats / I entry chain
    // colour-offset colour-size). Same draw order as jade2gltf's glTF, for a like-for-like judge.
    std::vector<uint8_t> packBytes;
    auto addPackScene = [&](const char *scenePath) -> bool {
        std::ifstream pf(packPath, std::ios::binary);
        packBytes.assign(std::istreambuf_iterator<char>(pf), {});
        pop_assets::Pack pack;
        if (packBytes.empty() || !pop_assets::indexPack(packBytes.data(), packBytes.size(), pack))
        {
            std::fprintf(stderr, "cannot index pack %s\n", packPath);
            return false;
        }
        struct Element { uint32_t offset, matid, entry, chain; };
        struct Inst { uint32_t entry, chain, offset, size; };
        struct Object { uint32_t geo, mm, lm; float m[16]; std::vector<Inst> inst; };
        std::map<uint32_t, std::vector<Element>> elements;
        std::vector<Object> objects;
        std::ifstream sf(scenePath);
        std::string line;
        while (std::getline(sf, line))
        {
            std::istringstream ss(line);
            std::string kind;
            ss >> kind;
            if (kind == "E")
            {
                std::string key;
                Element e{};
                ss >> key >> e.offset >> e.matid >> e.entry >> e.chain;
                elements[uint32_t(std::stoul(key, nullptr, 16))].push_back(e);
            }
            else if (kind == "O")
            {
                std::string name, geo, mm, lm;
                Object o{};
                ss >> name >> geo >> mm >> lm;
                o.geo = uint32_t(std::stoul(geo, nullptr, 16));
                o.mm = uint32_t(std::stoul(mm, nullptr, 16));
                o.lm = uint32_t(std::stoul(lm, nullptr, 16));
                for (float &f : o.m)
                    ss >> f;
                objects.push_back(o);
            }
            else if (kind == "I" && !objects.empty())
            {
                Inst in{};
                ss >> in.entry >> in.chain >> in.offset >> in.size;
                objects.back().inst.push_back(in);
            }
        }
        std::map<uint32_t, int> images; // texture key -> renderer image (-1: does not decode)
        auto image = [&](uint32_t key) {
            const auto it = images.find(key);
            if (it != images.end())
                return it->second;
            Image img;
            const int idx = pop_assets::decodeTexture(pack, key, img) ? r.addImage(img) : -1;
            images[key] = idx;
            ++imageTotal;
            return idx;
        };
        std::map<std::tuple<uint32_t, uint32_t, uint32_t>, int> materials; // (mm, matid, lightmap) -> material
        auto material = [&](uint32_t mm, uint32_t matid, uint32_t lm) {
            const auto k = std::make_tuple(mm, matid, lm);
            const auto it = materials.find(k);
            if (it != materials.end())
                return it->second;
            int idx = -1;
            pop_assets::SubMaterial sub;
            // as jade2gltf.material_base: a material only when the sub-material's texture decodes
            if (pop_assets::subMaterial(pack, mm, matid, sub) && pop_assets::textureFormat(pack, sub.texture))
            {
                Material mat;
                mat.baseImage = image(sub.texture);
                if (mat.baseImage >= 0)
                {
                    // the game's own blend table (render state flags, record +0x1C / +0x28)
                    pop_native::materialFromGsFlags(sub.flags, sub.ref, mat);
                    if (lm)
                        mat.lightmapImage = image(lm);
                    idx = r.addMaterial(mat);
                }
            }
            materials[k] = idx;
            return idx;
        };
        auto addBatch = [&](pop_assets::Batch &b, int mat, std::vector<int> &out) {
            Mesh mesh;
            mesh.vertices = std::move(b.vertices);
            mesh.indices = std::move(b.indices);
            mesh.material = mat;
            vertexTotal += mesh.vertices.size();
            triTotal += mesh.indices.size() / 3;
            out.push_back(r.addMesh(std::move(mesh)));
        };
        std::map<std::tuple<uint32_t, uint32_t, uint32_t>, std::vector<int>> meshCache; // (geo, mm, lm) -> meshes
        constexpr double kUvScale = 4096.0;
        for (const Object &o : objects)
        {
            const auto &els = elements[o.geo];
            const bool instanced = std::any_of(els.begin(), els.end(), [&](const Element &e) { return pop_assets::isInstanced(pack, e.offset); });
            std::vector<int> meshes;
            const auto key = std::make_tuple(o.geo, o.mm, o.lm);
            if (!instanced && meshCache.count(key))
                meshes = meshCache[key];
            else
            {
                for (const Element &e : els) // static elements (instanced objects: without the lightmap)
                {
                    std::vector<pop_assets::Batch> batches;
                    if (!pop_assets::convertElement(pack, e.offset, kUvScale, batches))
                        continue;
                    for (auto &b : batches)
                        addBatch(b, material(o.mm, e.matid, !instanced && b.hasUv2 ? o.lm : 0), meshes);
                }
                if (instanced)
                    for (const Element &e : els)
                    {
                        if (!pop_assets::isInstanced(pack, e.offset))
                            continue;
                        const uint8_t *colours = nullptr;
                        size_t colourBytes = 0;
                        for (const Inst &in : o.inst)
                            if (in.entry == e.entry && in.chain == e.chain && size_t(in.offset) + in.size <= packBytes.size())
                            {
                                colours = packBytes.data() + in.offset;
                                colourBytes = in.size;
                                break;
                            }
                        std::vector<pop_assets::Batch> batches;
                        if (pop_assets::convertInstanced(pack, e.offset, colours, colourBytes, kUvScale, batches))
                            for (auto &b : batches)
                                addBatch(b, material(o.mm, e.matid, 0), meshes);
                    }
                else
                    meshCache[key] = meshes;
            }
            Draw d;
            std::memcpy(d.model, o.m, sizeof d.model);
            for (int m : meshes)
            {
                d.mesh = m;
                draws.push_back(d);
            }
        }
        std::printf("pack %s: %zu objects, %zu geometry records, %zu materials\n", packPath, objects.size(), elements.size(), materials.size());
        return true;
    };
    // --frame: "PNF1", world -> clip[16], draws (model[16], batches: vertex/index counts, Vertex[],
    // u32[]), written by tools/overrides/pop_native_live.cpp; the frame's own camera replaces the
    // clip file's
    uint32_t glow[5] = {};
    bool glowOn = false, noGlow = false; // ColorDiffusion ran in the captured frame (PNF5); --noglow skips it
    uint32_t haze[2] = {};
    bool hazeOn = false, noHaze = false; // SmartBlur ran in the captured frame (HAZE trailer); --nohaze skips it
    uint32_t after[4] = {}; // MotionSmooth / FixBlend (AFTR trailer); FixBlend offline needs --prev <png> (the previous frame)
    const char *prevPath = nullptr;
    float zoom = 0.0f; // ZoomSmooth 0's strength (ZOOM trailer)
    for (int i = 4; i < argc; ++i)
    {
        if (!std::strcmp(argv[i], "--nohaze"))
            noHaze = true;
        if (!std::strcmp(argv[i], "--noglow"))
            noGlow = true;
        if (!std::strcmp(argv[i], "--prev") && i + 1 < argc)
            prevPath = argv[i + 1];
    }
    FrameLoader loader;           // PNF frames (files and live); live: keeps texture keys across frames
    Image liveGsImage;            // live: the game's own GS picture of the frame (POP_NATIVE_WINDOW_GS=d)
    std::vector<Image> liveGsImages; // all GS pictures sent with the frame (1..d boundaries later)
    auto addFrameFrom = [&](std::istream &f, const char *path) -> bool {
        FrameExtras extras;
        loader.vertexTotal = vertexTotal;
        loader.triTotal = triTotal;
        loader.imageTotal = imageTotal;
        const bool ok = loader.load(r, f, path, clip, draws, extras, !livePort);
        vertexTotal = loader.vertexTotal;
        triTotal = loader.triTotal;
        imageTotal = loader.imageTotal;
        std::memcpy(glow, extras.glow, sizeof glow);
        std::memcpy(haze, extras.haze, sizeof haze);
        std::memcpy(after, extras.after, sizeof after);
        zoom = extras.zoom;
        glowOn = extras.glowOn;
        hazeOn = extras.hazeOn;
        liveGsImages = std::move(extras.gsImages);
        liveGsImage = liveGsImages.empty() ? Image{} : liveGsImages.front();
        return ok;
    };
    auto addFrame = [&](const char *path) -> bool {
        std::ifstream f(path, std::ios::binary);
        return addFrameFrom(f, path);
    };
    if (framePath)
    {
        if (!addFrame(argv[1]))
        {
            std::fprintf(stderr, "cannot read frame %s\n", argv[1]);
            return 1;
        }
    }
    else if (packPath)
    {
        if (!addPackScene(argv[1]))
            return 1;
    }
    else if (!livePort)
        addScene(data, nullptr);
    std::vector<cgltf_data *> extra;
    for (int i = 4; i + 1 < argc; ++i)
        if (!std::strcmp(argv[i], "--add") || (!std::strcmp(argv[i], "--addfog") && i + 2 < argc))
        {
            float ownFog[2] = {255.0f, 0.0f};
            const bool withFog = !std::strcmp(argv[i], "--addfog");
            if (withFog)
                std::sscanf(argv[++i], "%f,%f", &ownFog[0], &ownFog[1]);
            cgltf_data *more = nullptr;
            if (cgltf_parse_file(&opt, argv[i + 1], &more) != cgltf_result_success || cgltf_load_buffers(&opt, more, argv[i + 1]) != cgltf_result_success)
            {
                std::fprintf(stderr, "cannot load %s\n", argv[i + 1]);
                return 1;
            }
            addScene(more, withFog ? ownFog : nullptr);
            extra.push_back(more);
        }
    if (!r.upload(error))
    {
        std::fprintf(stderr, "upload: %s\n", error.c_str());
        return 1;
    }
    for (int i = 4; i + 1 < argc; ++i)
        if (!std::strcmp(argv[i], "--fog")) // --fog a,b,R,G,B (GS fog constants and FOGCOL 0..255)
        {
            Fog fog;
            float c[3] = {};
            if (std::sscanf(argv[i + 1], "%f,%f,%f,%f,%f", &fog.a, &fog.b, &c[0], &c[1], &c[2]) == 5)
            {
                fog.enabled = true;
                for (int k = 0; k < 3; ++k)
                    fog.colour[k] = c[k] / 255.0f;
                r.setFog(fog);
            }
        }
    const auto t1 = std::chrono::steady_clock::now();
    if (windowed)
    {
        std::printf("%s: %zu draws, %zu triangles, %zu textures; loaded in %.0f ms\n", r.deviceName().c_str(), draws.size(), triTotal,
                    imageTotal, std::chrono::duration<double, std::milli>(t1 - t0).count());
        std::fflush(stdout);
        FreeCamera cam(clip);
        // --live: a thread accepts the game's connection and keeps the latest frame; the loop
        // rebuilds the scene from each new frame (the game's camera unless F switched to the free one)
        std::mutex liveMutex;
        std::vector<uint8_t> liveFrame;
        std::atomic<uint64_t> liveSeq{0}, liveConnection{0};
        uint64_t liveConnectionShown = 0;
        std::atomic<bool> liveQuit{false};
        std::thread liveThread;
        uint64_t liveShown = 0, liveFrames = 0, liveBytes = 0;
        double liveRebuildMs = 0;
        bool freeCam = !livePort;
        bool livePost = true; // G: the game's post effects (glow, haze) on the GPU
        bool showGs = false;  // T: show the game's own GS picture instead (POP_NATIVE_WINDOW_GS=1 in the game)
        if (!livePort && (glowOn || hazeOn))
        {
            // an exported frame in the window: its post effects on the GPU too
            PostEffects pe;
            pe.glow = glowOn && !noGlow;
            pe.levels = int(glow[0]);
            pe.fix = int(glow[1]);
            pe.mask = glow[2] != 0;
            pe.convolution = glow[3] != 0;
            pe.strength = int(glow[4]);
            pe.haze = hazeOn && !noHaze;
            pe.hazeColour = haze[0];
            std::memcpy(&pe.hazeFade, &haze[1], 4);
            pe.motionSmooth = after[0] & 1;
            pe.motionDx = int(after[1]);
            pe.motionDy = int(after[2]);
            pe.fixBlend = after[0] & 2 ? int(after[3]) : 0;
            pe.zoomSmooth = zoom;
            r.setPostEffects(pe);
        }
        if (livePort)
        {
            const pop_live::Socket server = pop_live::listenLocal(livePort);
            if (server == pop_live::kNoSocket)
            {
                std::fprintf(stderr, "cannot listen on 127.0.0.1:%d\n", livePort);
                return 1;
            }
            std::printf("live: waiting for the game on 127.0.0.1:%d (POP_NATIVE_WINDOW=%d)\n", livePort, livePort);
            std::fflush(stdout);
            liveThread = std::thread([&, server]() {
                while (!liveQuit)
                {
                    const pop_live::Socket c = accept(server, nullptr, nullptr);
                    if (c == pop_live::kNoSocket)
                        break;
                    {
                        std::lock_guard<std::mutex> lock(liveMutex);
                        ++liveConnection; // a new game: its texture keys start from nothing
                    }
                    std::printf("live: game connected\n");
                    std::fflush(stdout);
                    std::vector<uint8_t> buf;
                    while (!liveQuit && pop_live::recvFrame(c, buf))
                    {
                        std::lock_guard<std::mutex> lock(liveMutex);
                        liveFrame.swap(buf);
                        ++liveSeq;
                    }
                    pop_live::closeSocket(c);
                    std::printf("live: game disconnected\n");
                    std::fflush(stdout);
                }
                pop_live::closeSocket(server);
            });
            liveThread.detach();
        }
        long presented = 0;
        const auto runStart = std::chrono::steady_clock::now();
        bool running = true, looking = false;
        int shots = 0, frames = 0;
        auto last = std::chrono::steady_clock::now(), fpsStart = last;
        while (running)
        {
            if (livePort && liveSeq != liveShown)
            {
                std::vector<uint8_t> frame;
                bool newGame = false;
                {
                    std::lock_guard<std::mutex> lock(liveMutex);
                    frame = liveFrame;
                    liveShown = liveSeq;
                    newGame = liveConnection != liveConnectionShown;
                    liveConnectionShown = liveConnection;
                }
                const auto tr0 = std::chrono::steady_clock::now();
                r.reset(!newGame); // textures stay for the same game connection
                if (newGame)
                    loader.forgetImages();
                draws.clear();
                vertexTotal = triTotal = imageTotal = 0;
                std::istringstream in(std::string(reinterpret_cast<const char *>(frame.data()), frame.size()), std::ios::binary);
                if (!addFrameFrom(in, "live") || !r.upload(error))
                    std::fprintf(stderr, "live frame: %s\n", error.empty() ? "unreadable" : error.c_str());
                {
                    PostEffects pe;
                    pe.glow = livePost && glowOn;
                    pe.levels = int(glow[0]);
                    pe.fix = int(glow[1]);
                    pe.mask = glow[2] != 0;
                    pe.convolution = glow[3] != 0;
                    pe.strength = int(glow[4]);
                    pe.haze = livePost && hazeOn;
                    pe.hazeColour = haze[0];
                    std::memcpy(&pe.hazeFade, &haze[1], 4);
                    pe.motionSmooth = livePost && (after[0] & 1);
                    pe.motionDx = int(after[1]);
                    pe.motionDy = int(after[2]);
                    pe.fixBlend = livePost && (after[0] & 2) ? int(after[3]) : 0;
                    pe.zoomSmooth = livePost ? zoom : 0.0f;
                    r.setPostEffects(pe);
                    glowOn = hazeOn = false; // the next frame's trailer says again
                }
                if (showGs && !r.setOverlay(&liveGsImage, error))
                    std::fprintf(stderr, "overlay: %s\n", error.c_str());
                ++liveFrames;
                liveRebuildMs += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - tr0).count();
                liveBytes += frame.size();
            }
            static auto lastShot = std::chrono::steady_clock::now();
            if (livePort && liveShotSeconds > 0 && liveFrames &&
                std::chrono::duration<double>(std::chrono::steady_clock::now() - lastShot).count() >= liveShotSeconds)
            {
                lastShot = std::chrono::steady_clock::now();
                std::vector<uint8_t> px;
                uint32_t w = 0, h = 0;
                std::string path = std::string(argv[3]);
                const size_t dot = path.rfind('.');
                path = (dot == std::string::npos ? path : path.substr(0, dot)) + "_live_" + std::to_string(++shots) + ".png";
                if (std::getenv("POP_LIVE_DUMP"))
                {
                    // the frame as received (PNF6; textures sent in earlier frames are not in it)
                    std::lock_guard<std::mutex> lock(liveMutex);
                    std::ofstream(path.substr(0, path.size() - 4) + ".pnf", std::ios::binary)
                        .write(reinterpret_cast<const char *>(liveFrame.data()), std::streamsize(liveFrame.size()));
                }
                if (r.readPresented(px, w, h, error))
                {
                    std::vector<uint8_t> rgb(size_t(w) * h * 3);
                    for (size_t i = 0; i < size_t(w) * h; ++i)
                        std::memcpy(&rgb[i * 3], &px[i * 4], 3);
                    stbi_write_png(path.c_str(), int(w), int(h), 3, rgb.data(), int(w * 3));
                    std::printf("live: saved %s (game frame %llu, %zu draws; per frame %.1f ms rebuild, %.0f KB)\n", path.c_str(),
                                (unsigned long long)liveFrames, draws.size(), liveRebuildMs / double(liveFrames),
                                double(liveBytes) / double(liveFrames) / 1024.0);
                    for (size_t gi = 0; gi < liveGsImages.size() && !showGs; ++gi)
                    {
                        const Image &liveGsImage = liveGsImages[gi];
                        if (liveGsImage.width != w || liveGsImage.height != h)
                            continue;
                        // the game's GS picture of the same frame, and how far the native one is from it
                        std::vector<uint8_t> grgb(size_t(w) * h * 3);
                        double sum = 0;
                        size_t big = 0;
                        for (size_t i = 0; i < size_t(w) * h; ++i)
                            for (int c = 0; c < 3; ++c)
                            {
                                grgb[i * 3 + c] = liveGsImage.rgba[i * 4 + c];
                                const int d = std::abs(int(liveGsImage.rgba[i * 4 + c]) - int(px[i * 4 + c]));
                                sum += d;
                                big += d > 24;
                            }
                        const std::string gpath = path.substr(0, path.size() - 4) + "_gs" + std::to_string(gi + 1) + ".png";
                        stbi_write_png(gpath.c_str(), int(w), int(h), 3, grgb.data(), int(w * 3));
                        std::printf("live:   GS picture %zu boundaries later %s; native vs GS: mean difference %.2f, %.1f%% of values off by > 24\n",
                                    gi + 1, gpath.c_str(), sum / double(size_t(w) * h * 3), 100.0 * double(big) / double(size_t(w) * h * 3));
                    }
                    std::fflush(stdout);
                }
            }
            SDL_Event e;
            while (SDL_PollEvent(&e))
            {
                if (e.type == SDL_EVENT_QUIT)
                    running = false;
                else if (e.type == SDL_EVENT_MOUSE_BUTTON_DOWN && e.button.button == SDL_BUTTON_LEFT)
                    looking = true;
                else if (e.type == SDL_EVENT_MOUSE_BUTTON_UP && e.button.button == SDL_BUTTON_LEFT)
                    looking = false;
                else if (e.type == SDL_EVENT_MOUSE_MOTION && looking)
                    cam.turn(-e.motion.xrel * 0.004f, -e.motion.yrel * 0.004f);
                else if (e.type == SDL_EVENT_KEY_DOWN && !e.key.repeat)
                {
                    const SDL_Keycode k = e.key.key;
                    if (k == SDLK_ESCAPE)
                        running = false;
                    else if (k == SDLK_R)
                        cam.reset();
                    else if (k == SDLK_G && livePort)
                        livePost = !livePost;
                    else if (k == SDLK_T && livePort)
                    {
                        showGs = !showGs;
                        if (!r.setOverlay(showGs ? &liveGsImage : nullptr, error))
                            std::fprintf(stderr, "overlay: %s\n", error.c_str());
                    }
                    else if (k == SDLK_F && livePort)
                    {
                        freeCam = !freeCam;
                        if (freeCam)
                            cam = FreeCamera(clip); // start from the game's current view
                    }
                    else if (k >= SDLK_1 && k <= SDLK_4)
                        scale = static_cast<uint32_t>(k - SDLK_1 + 1);
                    else if (k == SDLK_P)
                    {
                        float c[16];
                        cam.clip(c);
                        std::vector<uint8_t> px;
                        const uint32_t w = 640 * scale, h = 448 * scale;
                        std::string path = std::string(argv[3]);
                        const size_t dot = path.rfind('.');
                        path = (dot == std::string::npos ? path : path.substr(0, dot)) + "_" + std::to_string(++shots) + ".png";
                        if (r.renderOffscreen(w, h, c, draws, px, error))
                        {
                            std::vector<uint8_t> rgb(size_t(w) * h * 3);
                            for (size_t i = 0; i < size_t(w) * h; ++i)
                                std::memcpy(&rgb[i * 3], &px[i * 4], 3);
                            stbi_write_png(path.c_str(), int(w), int(h), 3, rgb.data(), int(w * 3));
                            std::printf("saved %s (%ux%u)\n", path.c_str(), w, h);
                        }
                    }
                }
            }
            const auto now = std::chrono::steady_clock::now();
            const float dt = std::chrono::duration<float>(now - last).count();
            last = now;
            const bool *keys = SDL_GetKeyboardState(nullptr);
            const float speed = (keys[SDL_SCANCODE_LSHIFT] || keys[SDL_SCANCODE_RSHIFT] ? 20.0f : 5.0f) * dt;
            cam.move(speed * (keys[SDL_SCANCODE_W] - keys[SDL_SCANCODE_S]), speed * (keys[SDL_SCANCODE_D] - keys[SDL_SCANCODE_A]),
                     speed * (keys[SDL_SCANCODE_E] - keys[SDL_SCANCODE_Q]));
            int ww = 0, wh = 0;
            SDL_GetWindowSizeInPixels(window, &ww, &wh);
            float c[16];
            if (freeCam)
                cam.clip(c);
            else
                std::memcpy(c, clip, sizeof c);
            if (!r.present(uint32_t(ww), uint32_t(wh), 640 * scale, 448 * scale, c, draws, error))
            {
                std::fprintf(stderr, "present: %s\n", error.c_str());
                break;
            }
            ++frames;
            if (maxFrames >= 0 && ++presented >= maxFrames)
            {
                if (presentShot)
                {
                    // --present-shot: the last presented frame (GPU post effects included)
                    std::vector<uint8_t> px;
                    uint32_t w = 0, h = 0;
                    if (r.readPresented(px, w, h, error))
                    {
                        std::vector<uint8_t> rgb(size_t(w) * h * 3);
                        for (size_t i = 0; i < size_t(w) * h; ++i)
                            std::memcpy(&rgb[i * 3], &px[i * 4], 3);
                        stbi_write_png(presentShot, int(w), int(h), 3, rgb.data(), int(w * 3));
                        std::printf("saved the presented frame %s (%ux%u)\n", presentShot, w, h);
                    }
                }
                const float total = std::chrono::duration<float>(std::chrono::steady_clock::now() - runStart).count();
                std::printf("%ld frames presented in %.2f s (%.0f fps, vsync), window %dx%d, internal %ux%u\n", presented, total,
                            presented / total, ww, wh, 640 * scale, 448 * scale);
                running = false;
            }
            const float secs = std::chrono::duration<float>(now - fpsStart).count();
            if (secs >= 1.0f)
            {
                char title[128];
                if (livePort)
                    std::snprintf(title, sizeof title, "PoP native (live) - %ux%u - %.0f fps, game frames %llu - %zu draws%s", 640 * scale,
                                  448 * scale, frames / secs, (unsigned long long)liveFrames, draws.size(),
                                  freeCam ? " - free camera (F: game camera)" : " - game camera (F: free camera)");
                else
                    std::snprintf(title, sizeof title, "PoP native view - %ux%u - %.0f fps%s", 640 * scale, 448 * scale, frames / secs,
                                  cam.moved() ? "" : " - game camera");
                SDL_SetWindowTitle(window, title);
                frames = 0;
                fpsStart = now;
            }
        }
        liveQuit = true;
        cgltf_free(data);
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 0;
    }
    std::vector<uint8_t> rgba;
    std::vector<float> depth;
    std::vector<uint32_t> gsZ;
    const char *depthPath = nullptr, *gsZPath = nullptr;
    for (int i = 4; i + 1 < argc; ++i)
        if (!std::strcmp(argv[i], "--depth"))
            depthPath = argv[i + 1];
        else if (!std::strcmp(argv[i], "--gsz"))
            gsZPath = argv[i + 1];
    if (!r.renderOffscreen(width, height, clip, draws, rgba, error, depthPath ? &depth : nullptr, gsZPath ? &gsZ : nullptr))
    {
        std::fprintf(stderr, "render: %s\n", error.c_str());
        return 1;
    }
    if (const char *alphaOut = std::getenv("POP_VIEW_ALPHA")) // debug: the frame's alpha (glow mask) before the glow, grey
    {
        std::vector<uint8_t> grey(size_t(width) * height * 3);
        for (size_t i = 0; i < size_t(width) * height; ++i)
            grey[3 * i] = grey[3 * i + 1] = grey[3 * i + 2] = rgba[4 * i + 3];
        stbi_write_png(alphaOut, int(width), int(height), 3, grey.data(), int(width * 3));
    }
    if (zoom > 0.0f) // ZoomSmooth 0, before the glow
    {
        pop_postfx::Surface frame, temp;
        frame.w = int(width);
        frame.h = int(height);
        frame.rgba = std::move(rgba);
        pop_postfx::runBlits(pop_postfx::zoomSmoothBlits(zoom, frame.w, frame.h, std::max(1, frame.w / 640)), frame, temp);
        rgba = std::move(frame.rgba);
    }
    if (glowOn && !noGlow)
        pop_postfx::colorDiffusion(rgba, int(width), int(height), int(glow[0]), int(glow[1]), glow[2] != 0, glow[3] != 0, int(glow[4]));
    {
        // MotionSmooth, then FixBlend over the previous frame (--prev: offline, the save's displayed picture)
        pop_postfx::Surface frame, temp, prev;
        frame.w = int(width);
        frame.h = int(height);
        frame.rgba = std::move(rgba);
        std::vector<pop_postfx::Blit> blits;
        if (after[0] & 1)
            blits = pop_postfx::motionSmoothBlits(int(after[1]), int(after[2]), frame.w, frame.h, std::max(1, frame.w / 640));
        if (after[0] & 2)
        {
            int pw = 0, ph = 0, pc = 0;
            stbi_uc *px = prevPath ? stbi_load(prevPath, &pw, &ph, &pc, 4) : nullptr;
            if (px && pw == frame.w && ph == frame.h)
            {
                prev.w = pw;
                prev.h = ph;
                prev.rgba.assign(px, px + size_t(pw) * ph * 4);
                blits.push_back(pop_postfx::fixBlendBlit(int(after[3]), frame.w, frame.h));
            }
            else
                std::fprintf(stderr, "FixBlend (FIX %u) skipped: no --prev picture of %ux%u\n", after[3], width, height);
            stbi_image_free(px);
        }
        pop_postfx::runBlits(blits, frame, temp, &prev);
        rgba = std::move(frame.rgba);
    }
    if (hazeOn && !noHaze)
    {
        pop_postfx::Surface frame, work;
        frame.w = int(width);
        frame.h = int(height);
        frame.rgba = std::move(rgba);
        float fade = 0;
        std::memcpy(&fade, &haze[1], 4);
        pop_postfx::smartBlur(frame, work, haze[0], fade, 11, false, int(width) / 640);
        rgba = std::move(frame.rgba);
    }
    const auto t2 = std::chrono::steady_clock::now();
    // opaque output (the colour target's alpha is not meaningful for the comparison)
    std::vector<uint8_t> rgb(size_t(width) * height * 3);
    for (size_t i = 0; i < size_t(width) * height; ++i)
        std::memcpy(&rgb[i * 3], &rgba[i * 4], 3);
    if (!stbi_write_png(argv[3], static_cast<int>(width), static_cast<int>(height), 3, rgb.data(), static_cast<int>(width * 3)))
    {
        std::fprintf(stderr, "cannot write %s\n", argv[3]);
        return 1;
    }
    if (depthPath) // raw little-endian float32 view depth per pixel, row-major
    {
        std::ofstream df(depthPath, std::ios::binary);
        df.write(reinterpret_cast<const char *>(depth.data()), static_cast<std::streamsize>(depth.size() * sizeof(float)));
    }
    if (gsZPath) // raw little-endian uint32 GS Z (Z24) per pixel, row-major, before the post effects
    {
        std::ofstream zf(gsZPath, std::ios::binary);
        zf.write(reinterpret_cast<const char *>(gsZ.data()), static_cast<std::streamsize>(gsZ.size() * sizeof(uint32_t)));
    }
    std::printf("%s: %zu draws, %zu vertices, %zu triangles, %zu textures; load+upload %.0f ms, render %.1f ms -> %s (%ux%u)\n",
                r.deviceName().c_str(), draws.size(), vertexTotal, triTotal, imageTotal,
                std::chrono::duration<double, std::milli>(t1 - t0).count(), std::chrono::duration<double, std::milli>(t2 - t1).count(),
                argv[3], width, height);
    cgltf_free(data);
    return 0;
}
