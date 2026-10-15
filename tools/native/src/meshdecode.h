// Mesh packet decoding for the native renderer (Stage B step 2): C++ port of the decoding functions
// in tools/jade2gltf.py (element_parts, decode_packet, variant3_batches, skinned_batches,
// batch_mesh). Fixed API: the implementation (meshdecode.cpp) must match the Python reference
// exactly on every element of every world pack (tools/llm_port/meshdecode/judge).
//
// Conventions shared by all functions:
// - `d`/`len` is a whole unpacked world pack (little-endian). Offsets are byte offsets into it.
// - Reading past `len` must never crash. Where the Python reference slices bytes (d[a:b]), bytes
//   beyond the end are simply missing (the result is shorter); where it unpacks fixed-size values
//   (struct.unpack_from) past the end, Python raises: return false from the function (the judge
//   then ignores that section).
#pragma once
#include <cstdint>
#include <map>
#include <vector>

namespace pop_mesh
{
    struct Block
    {
        uint32_t index; // element_parts: 'v' (bits/variant) for LOD blobs, pass index for passes
        uint32_t offset;
        uint32_t size;
    };

    struct ElementParts
    {
        uint32_t boneCount = 0;      // nb: number of bone palette bytes
        std::vector<Block> lods;     // (v, offset, size)
        std::vector<Block> passes;   // (pass index, offset, size)
    };
    // element_parts(d, o)
    bool elementParts(const uint8_t *d, size_t len, uint32_t o, ElementParts &out);

    struct Unpack
    {
        uint32_t addr = 0;           // VU address (imm & 0x3FF)
        std::vector<uint8_t> raw;    // EL[format] * count bytes (shorter if the pack ends)
        uint32_t count = 0;          // n
    };
    using Batch = std::map<uint32_t, Unpack>; // UNPACK format (cmd & 0xF) -> data
    // decode_packet(d, o, size, blobs); blobs maps block index -> (offset, size), or nullptr for None
    bool decodePacket(const uint8_t *d, size_t len, uint32_t o, uint32_t size,
                      const std::map<uint32_t, std::pair<uint32_t, uint32_t>> *blobs, std::vector<Batch> &out);

    struct Variant3Batch
    {
        std::vector<float> positions; // 3 per vertex
        std::vector<int16_t> uvs;     // 2 per vertex (s, t)
        std::vector<int16_t> flags;   // 1 per vertex
    };
    // variant3_batches(d, lo, ls)
    bool variant3Batches(const uint8_t *d, size_t len, uint32_t lo, uint32_t ls, std::vector<Variant3Batch> &out);

    struct SkinnedBatch
    {
        std::vector<float> positions;  // 3 per vertex
        std::vector<int32_t> uvs;      // 2 per vertex (Python ints; (0, 0) when absent)
        std::vector<int32_t> flags;    // 1 per vertex
        std::vector<std::vector<int32_t>> bones; // per vertex: the bytes d[3i:3i+3] of the 0x03 block, each // 4
                                                 // (fewer than 3 if the block is short, as the Python slice; (0, 0, 0) when absent)
        std::vector<float> weights;    // 3 per vertex ((1.0, 0, 0) when absent)
        bool hasNormals = false;       // Python: normals is not None
        std::vector<int32_t> normals;  // 3 per vertex (signed bytes) when hasNormals
        bool hasColours = false;       // Python: colours is not None
        std::vector<double> colours;   // 4 per vertex (min(byte / 128.0, 1.0); fewer if the data is short) when hasColours
    };
    // skinned_batches(d, o, size)
    bool skinnedBatches(const uint8_t *d, size_t len, uint32_t o, uint32_t size, std::vector<SkinnedBatch> &out);

    struct Mesh
    {
        std::vector<float> positions;  // 3 per vertex
        std::vector<double> uvs;       // 2 per vertex: s / uvscale, t / uvscale (double, as Python)
        std::vector<double> colours;   // 4 per vertex: min(byte / 128.0, 1.0)
        std::vector<uint32_t> tris;    // 3 per triangle
    };
    // batch_mesh(b, uvscale); returns false when Python returns None (no format 8) or raises
    bool batchMesh(const Batch &b, double uvscale, Mesh &out);
}
