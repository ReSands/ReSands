// pop_native_view: renders a level converted by tools/jade2gltf.py (--place) with the native Vulkan
// renderer through the game's own camera, to a PNG that tools/imgscore.py can compare with a GS
// frame of the same moment.
//
// Usage: pop_native_view <scene.gltf> <clip file> <out.png> [--nearest] [--width W --height H]
//   clip file: the game's world -> clip matrix, 16 comma- or space-separated floats (row vectors, as
//   graphics system +0x240 holds it; POP_CAMLOG / POP_FRAMECAP "capture" lines carry it).
#include "native_renderer.h"

#define CGLTF_IMPLEMENTATION
#include <cgltf.h>
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#include <stb_image.h>
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image_write.h>

#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
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
        std::fprintf(stderr, "usage: %s <scene.gltf> <clip file> <out.png> [--nearest] [--width W --height H]\n", argv[0]);
        return 2;
    }
    Options options;
    uint32_t width = 640, height = 448;
    for (int i = 4; i < argc; ++i)
    {
        if (!std::strcmp(argv[i], "--nearest"))
            options.linearFilter = false;
        else if (!std::strcmp(argv[i], "--width") && i + 1 < argc)
            width = static_cast<uint32_t>(std::atoi(argv[++i]));
        else if (!std::strcmp(argv[i], "--height") && i + 1 < argc)
            height = static_cast<uint32_t>(std::atoi(argv[++i]));
    }
    float clip[16];
    if (!readClip(argv[2], clip))
    {
        std::fprintf(stderr, "cannot read 16 floats from %s\n", argv[2]);
        return 1;
    }
    const auto t0 = std::chrono::steady_clock::now();
    cgltf_options opt{};
    cgltf_data *data = nullptr;
    if (cgltf_parse_file(&opt, argv[1], &data) != cgltf_result_success || cgltf_load_buffers(&opt, data, argv[1]) != cgltf_result_success)
    {
        std::fprintf(stderr, "cannot load %s\n", argv[1]);
        return 1;
    }

    Renderer r;
    std::string error;
    if (!r.init(options, error))
    {
        std::fprintf(stderr, "Vulkan: %s\n", error.c_str());
        return 1;
    }

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
        mat.alphaMode = gm.alpha_mode == cgltf_alpha_mode_mask    ? AlphaMode::Mask
                        : gm.alpha_mode == cgltf_alpha_mode_blend ? AlphaMode::Blend
                                                                  : AlphaMode::Opaque;
        mat.cutoff = gm.alpha_cutoff;
        materialIndex[i] = r.addMaterial(mat);
    }

    // one renderer mesh per glTF primitive
    std::map<const cgltf_mesh *, std::vector<int>> meshPrims;
    size_t vertexTotal = 0, triTotal = 0;
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

    std::vector<Draw> draws;
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
        for (int m : meshPrims[n.mesh])
        {
            d.mesh = m;
            draws.push_back(d);
        }
    }
    if (!r.upload(error))
    {
        std::fprintf(stderr, "upload: %s\n", error.c_str());
        return 1;
    }
    const auto t1 = std::chrono::steady_clock::now();
    std::vector<uint8_t> rgba;
    if (!r.renderOffscreen(width, height, clip, draws, rgba, error))
    {
        std::fprintf(stderr, "render: %s\n", error.c_str());
        return 1;
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
    std::printf("%s: %zu draws, %zu vertices, %zu triangles, %zu textures; load+upload %.0f ms, render %.1f ms -> %s (%ux%u)\n",
                r.deviceName().c_str(), draws.size(), vertexTotal, triTotal, data->images_count,
                std::chrono::duration<double, std::milli>(t1 - t0).count(), std::chrono::duration<double, std::milli>(t2 - t1).count(),
                argv[3], width, height);
    cgltf_free(data);
    return 0;
}
