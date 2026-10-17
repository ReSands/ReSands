// Asset conversion for the native renderer (Stage B step 2): Jade world-pack records -> renderer
// meshes, textures and materials. C++ port of the static-world parts of tools/jade2gltf.py and
// tools/jadetex.py (PS2 data only). The same functions serve the offline viewer (packs read from
// disk) and, later, the runtime (the pack buffer the game's loader reads, in guest memory).
// Characters (skinned meshes) are not converted here yet.
#pragma once
#include "meshdecode.h"
#include "native_renderer.h"

#include <cstdint>
#include <unordered_map>
#include <vector>

namespace pop_assets
{
    // An unpacked world pack: records [u32 size][0xEEFFC099][u32 key][data].
    struct Pack
    {
        const uint8_t *d = nullptr;
        size_t len = 0;
        std::unordered_map<uint32_t, std::pair<uint32_t, uint32_t>> records; // key -> (data offset, size)
    };
    bool indexPack(const uint8_t *d, size_t len, Pack &out);

    // PS2 texture records: format 3 (8-bit indices, GS-swizzled, palette in a separate record) and
    // format 6 (lightmap page: 4-bit, uploaded as PSMCT32, read back as PSMT4 with a greyscale
    // CLUT: index i -> grey 17 * i). RGBA out; texture alpha 0x80 -> 255.
    bool isTexture(const Pack &pack, uint32_t key);
    uint32_t textureFormat(const Pack &pack, uint32_t key); // 0 when not a texture
    bool decodeTexture(const Pack &pack, uint32_t key, pop_native::Image &out);

    // Multi-material (record kind 4) entry -> sub-material (kind 5): texture key and blend state.
    struct SubMaterial
    {
        uint32_t texture = 0;
        pop_native::AlphaMode mode = pop_native::AlphaMode::Opaque;
        float cutoff = 0.5f; // MASK: (+0x28 top byte) / 255 on the doubled alpha scale
        uint32_t flags = 0, ref = 0; // +0x1C (the game's render state flags) and +0x28: pop_native::materialFromGsFlags
    };
    bool subMaterial(const Pack &pack, uint32_t multiMaterial, uint32_t index, SubMaterial &out);

    // One drawable batch: triangle list over its own vertices.
    struct Batch
    {
        std::vector<pop_native::Vertex> vertices;
        std::vector<uint32_t> indices;
        bool hasUv2 = false; // lightmap UVs present (pass 3)
    };

    // A static mesh element (CJMeshElement record at `offset`): its first pass packet's batches,
    // with the lightmap pass's UVs where present. Texture UVs are raw / uvscale for inline packets
    // and raw / (uvscale / 8) for packets referencing data blocks. Returns false for elements this
    // converter does not draw (skinned, instanced, no pass).
    bool convertElement(const Pack &pack, uint32_t offset, double uvscale, std::vector<Batch> &out);

    // A skinned element (bone palette, pass packet with bone slots and weights): bind-pose batches.
    // Per vertex: up to 3 (slot, weight) pairs; slot k >= 1 = the k-th bone of the element's palette
    // (0 = unused). UVs at 1/512. Posing is the caller's (the bone matrices come from the game).
    struct SkinnedBatch
    {
        std::vector<float> positions; // 3 per vertex
        std::vector<float> uvs;       // 2 per vertex
        std::vector<float> colours;   // 4 per vertex (1.0 when the batch has none)
        std::vector<int32_t> slots;   // 3 per vertex
        std::vector<float> weights;   // 3 per vertex
        std::vector<float> normals;   // 3 per vertex (bind space, unit length) when the batch has normals, else empty
        std::vector<uint32_t> indices;
    };
    bool convertSkinned(const Pack &pack, uint32_t offset, std::vector<SkinnedBatch> &out);
    // The same from a skinned pass packet at `packet` (`size` bytes, inline VIF data: the file's packet,
    // or the one in game memory after load, whose tags hold no block references).
    bool skinnedFromPacket(const uint8_t *d, size_t len, uint32_t packet, uint32_t size, std::vector<SkinnedBatch> &out);

    // An instanced element (LOD variant 3 block, no pass packet): the shared positions/UVs, with
    // the game object's per-vertex colour block (`colours`, may be null: white).
    bool isInstanced(const Pack &pack, uint32_t offset);
    bool convertInstanced(const Pack &pack, uint32_t offset, const uint8_t *colours, size_t colourBytes, double uvscale,
                          std::vector<Batch> &out);
}
