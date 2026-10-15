// Mesh packet decoding (Stage B step 2): C++ port of tools/jade2gltf.py's element_parts,
// decode_packet, variant3_batches, skinned_batches and batch_mesh. Written by a local model
// (Qwen3.6-35B-A3B, tools/llm_port/meshdecode, 15 Oct 2026) against the fixed API in meshdecode.h,
// judged identical to the Python on 1,800 elements of 7 world packs; reviewed and hardened for
// corrupt input (64-bit offsets, no pointers past the end, truncated skinned data -> false).
#include "meshdecode.h"
#include <cstdint>
#include <map>
#include <vector>
#include <algorithm>
#include <functional>
#include <cstring>

namespace pop_mesh {

namespace {
    bool readU32(const uint8_t* d, size_t len, size_t off, uint32_t& val) {
        if (off + 4 > len) return false;
        val = d[off] | (d[off+1] << 8) | (d[off+2] << 16) | (d[off+3] << 24);
        return true;
    }
    bool readF32(const uint8_t* d, size_t len, size_t off, float& val) {
        if (off + 4 > len) return false;
        std::memcpy(&val, d + off, 4);
        return true;
    }
    bool readS16(const uint8_t* d, size_t len, size_t off, int16_t& val) {
        if (off + 2 > len) return false;
        val = (int16_t)(d[off] | (d[off+1] << 8));
        return true;
    }

    const std::map<uint8_t, uint8_t> EL = {
        {0, 4}, {1, 2}, {2, 1}, {4, 8}, {5, 4}, {6, 2}, {8, 12}, {9, 6},
        {0xA, 3}, {0xC, 16}, {0xD, 8}, {0xE, 4}, {0xF, 2}
    };
}

bool elementParts(const uint8_t *d, size_t len, uint32_t o, ElementParts &out) {
    uint64_t q = uint64_t(o) + 36; // 64-bit: sizes read from the data must not wrap
    uint32_t nb;
    if (!readU32(d, len, q, nb)) return false;
    q += 4 + nb;
    
    uint32_t n;
    if (!readU32(d, len, q, n)) return false;
    q += 4;
    
    for (uint32_t i = 0; i < n; ++i) {
        uint32_t v, s;
        if (!readU32(d, len, q, v)) return false;
        if (!readU32(d, len, q + 4, s)) return false;
        out.lods.push_back({v, static_cast<uint32_t>(q + 8), s});
        q += 8 + s;
    }
    
    uint32_t n2;
    if (!readU32(d, len, q, n2)) return false;
    q += 4;
    
    for (uint32_t i = 0; i < n2; ++i) {
        uint32_t idx, s;
        if (!readU32(d, len, q, idx)) return false;
        if (!readU32(d, len, q + 4, s)) return false;
        out.passes.push_back({idx, static_cast<uint32_t>(q + 8), s});
        q += 8 + s;
    }
    out.boneCount = nb;
    return true;
}

bool decodePacket(const uint8_t *d, size_t len, uint32_t o, uint32_t size,
                  const std::map<uint32_t, std::pair<uint32_t, uint32_t>> *blobs, std::vector<Batch> &out) {
    std::map<uint32_t, Unpack> cur;
    std::vector<Batch> batches;

    std::function<bool(size_t v, size_t stop, const std::pair<uint32_t, uint32_t>* ext)> vif = [&](size_t v, size_t stop, const std::pair<uint32_t, uint32_t>* ext) -> bool {
        while (v < stop) {
            uint32_t w;
            if (!readU32(d, len, v, w)) return false;
            uint8_t cmd = (w >> 24) & 0x7F;
            uint8_t num = (w >> 16) & 0xFF;
            uint16_t imm = w & 0xFFFF;
            
            if (cmd >= 0x60) {
                uint8_t f = cmd & 0xF;
                auto el_it = EL.find(f);
                if (el_it == EL.end()) return false;
                uint8_t el = el_it->second;
                uint32_t n = num ? num : 256;
                size_t nbytes = ((size_t)el * n + 3) / 4 * 4;
                
                if (ext && v + 4 >= stop) {
                    uint32_t src = ext->first;
                    size_t raw_len = 0;
                    if (src < len) {
                        raw_len = std::min((size_t)el * n, len - src);
                    }
                    if (cur.count(f)) {
                        batches.push_back(cur);
                        cur.clear();
                    }
                    Unpack u;
                    u.addr = imm & 0x3FF;
                    if (raw_len)
                        u.raw.assign(d + src, d + src + raw_len);
                    u.count = n;
                    cur[f] = u;
                    
                    size_t rest = src + nbytes;
                    if (rest + 4 <= ext->first + ext->second) {
                        if (!vif(rest, ext->first + ext->second, nullptr)) return false;
                    }
                    v = stop;
                    continue;
                }
                
                size_t data_start = v + 4;
                size_t raw_len2 = 0;
                if (data_start < len) {
                    raw_len2 = std::min((size_t)el * n, len - data_start);
                }
                if (cur.find(f) == cur.end()) {
                    Unpack u;
                    u.addr = imm & 0x3FF;
                    if (raw_len2)
                        u.raw.assign(d + data_start, d + data_start + raw_len2);
                    u.count = n;
                    cur[f] = u;
                }
                v += 4 + nbytes;
            } else {
                if (cmd == 0x17) {
                    batches.push_back(cur);
                    cur.clear();
                }
                size_t skip = 4;
                if (cmd == 0x20) skip += 4;
                else if (cmd == 0x30 || cmd == 0x31) skip += 16;
                if (cmd == 0x50 || cmd == 0x51) skip += 16 * (imm ? imm : 65536);
                v += skip;
            }
        }
        return true;
    };

    size_t p = o;
    size_t end = o + size;
    while (p + 16 <= end) {
        uint32_t lo, addr;
        if (!readU32(d, len, p, lo)) return false;
        if (!readU32(d, len, p + 4, addr)) return false;
        
        uint16_t qwc = lo & 0xFFFF;
        uint8_t tid = (lo >> 28) & 7;
        
        if ((tid == 0 || tid == 3) && blobs) {
            uint8_t idx = addr >> 24;
            auto it = blobs->find(idx);
            if (it != blobs->end()) {
                uint32_t off = addr & 0xFFFFFF;
                size_t ext_size = (size_t)qwc * 16;
                if (off + ext_size <= it->second.second) {
                    std::pair<uint32_t, uint32_t> ext = {it->second.first + off, ext_size};
                    if (!vif(p + 8, p + 16, &ext)) return false;
                }
            }
            p += 16;
            if (tid == 0) break;
            continue;
        }
        
        size_t stop = p + 16 + 16 * qwc;
        if (!vif(p + 8, std::min(stop, end), nullptr)) return false;
        p = stop;
        if (tid == 6 || tid == 7) break;
    }
    
    if (!cur.empty()) {
        batches.push_back(cur);
    }
    
    out = batches;
    return true;
}

bool variant3Batches(const uint8_t *d, size_t len, uint32_t lo, uint32_t ls, std::vector<Variant3Batch> &out) {
    size_t p = lo;
    size_t end = lo + (size_t)ls;
    while (p + 4 <= end) {
        uint32_t w;
        if (!readU32(d, len, p, w)) return false;
        uint8_t cmd = (w >> 24) & 0x7F;
        uint8_t num = (w >> 16) & 0xFF;
        if ((cmd == 0x68 || cmd == 0x78) && num != 0) {
            uint32_t n = num;
            size_t uv0 = p - ((6 * n + 3) / 4 * 4);
            if (uv0 < lo) {
                return true;
            }
            if (p + 4 + 12 * n > len) return false;
            if (uv0 + 6 * n > len) return false;
            
            Variant3Batch batch;
            batch.positions.reserve(3 * n);
            batch.uvs.reserve(2 * n);
            batch.flags.reserve(n);
            
            for (uint32_t i = 0; i < n; ++i) {
                float px, py, pz;
                if (!readF32(d, len, p + 4 + 12 * i, px) ||
                    !readF32(d, len, p + 4 + 12 * i + 4, py) ||
                    !readF32(d, len, p + 4 + 12 * i + 8, pz)) return false;
                batch.positions.push_back(px);
                batch.positions.push_back(py);
                batch.positions.push_back(pz);
                
                int16_t a, b, c;
                if (!readS16(d, len, uv0 + 6 * i, a) ||
                    !readS16(d, len, uv0 + 6 * i + 2, b) ||
                    !readS16(d, len, uv0 + 6 * i + 4, c)) return false;
                batch.uvs.push_back(a);
                batch.uvs.push_back(b);
                batch.flags.push_back(c);
            }
            out.push_back(batch);
            p += 4 + 12 * n;
            continue;
        }
        p += 4;
    }
    return true;
}

bool skinnedBatches(const uint8_t *d, size_t len, uint32_t o, uint32_t size, std::vector<SkinnedBatch> &out) {
    struct RawData {
        std::vector<uint8_t> raw;
        uint32_t count = 0;
        uint8_t stride = 0;
    };
    std::map<uint32_t, RawData> cur;
    std::vector<SkinnedBatch> batches;
    bool truncated = false; // Python's struct.unpack would raise: the function returns false

    auto flush = [&]() {
        if (cur.find(0x53) != cur.end()) {
            const auto& pk = cur[0x53];
            uint32_t n = pk.count;
            uint8_t stride = pk.stride;
            
            SkinnedBatch b;
            b.positions.reserve(3 * n);
            if (pk.raw.size() < stride * n) { truncated = true; cur.clear(); return; }
            for (uint32_t i = 0; i < n; ++i) {
                float px, py, pz;
                if (!readF32(pk.raw.data(), pk.raw.size(), stride * i, px) ||
                    !readF32(pk.raw.data(), pk.raw.size(), stride * i + 4, py) ||
                    !readF32(pk.raw.data(), pk.raw.size(), stride * i + 8, pz)) { truncated = true; cur.clear(); return; }
                b.positions.push_back(px);
                b.positions.push_back(py);
                b.positions.push_back(pz);
            }
            
            b.uvs.assign(n * 2, 0);
            b.flags.assign(n, 0);
            auto uk_it = cur.find(0x7B);
            if (uk_it != cur.end()) {
                const auto& uk = uk_it->second;
                uint32_t m = std::min(n, uk.count);
                if (uk.raw.size() < 6 * m) { truncated = true; cur.clear(); return; }
                for (uint32_t i = 0; i < m; ++i) {
                    int16_t a, b_uv, c;
                    if (!readS16(uk.raw.data(), uk.raw.size(), 6 * i, a) ||
                        !readS16(uk.raw.data(), uk.raw.size(), 6 * i + 2, b_uv) ||
                        !readS16(uk.raw.data(), uk.raw.size(), 6 * i + 4, c)) { truncated = true; cur.clear(); return; }
                    b.uvs[i * 2] = a;
                    b.uvs[i * 2 + 1] = b_uv;
                    b.flags[i] = c;
                }
            }
            
            if (cur.find(0x03) != cur.end()) {
                const auto& bones_raw = cur[0x03];
                if (bones_raw.raw.size() < 3 * n) { truncated = true; cur.clear(); return; }
                b.bones.resize(n);
                for (uint32_t i = 0; i < n; ++i) {
                    uint8_t bx = bones_raw.raw[3 * i];
                    uint8_t by = bones_raw.raw[3 * i + 1];
                    uint8_t bz = bones_raw.raw[3 * i + 2];
                    b.bones[i] = {(int32_t)bx / 4, (int32_t)by / 4, (int32_t)bz / 4};
                }
            } else {
                b.bones.assign(n, {0, 0, 0});
            }
            
            if (cur.find(0x2B) != cur.end()) {
                const auto& w = cur[0x2B];
                if (w.raw.size() < 12 * n) { truncated = true; cur.clear(); return; }
                b.weights.reserve(3 * n);
                for (uint32_t i = 0; i < n; ++i) {
                    float w1, w2, w3;
                    if (!readF32(w.raw.data(), w.raw.size(), 12 * i, w1) ||
                        !readF32(w.raw.data(), w.raw.size(), 12 * i + 4, w2) ||
                        !readF32(w.raw.data(), w.raw.size(), 12 * i + 8, w3)) { truncated = true; cur.clear(); return; }
                    b.weights.push_back(w1);
                    b.weights.push_back(w2);
                    b.weights.push_back(w3);
                }
            } else {
                b.weights.assign(3 * n, 0.0f);
                for (uint32_t i = 0; i < n; ++i) b.weights[i*3] = 1.0f;
            }
            
            b.hasNormals = false;
            b.hasColours = false;
            auto nk_it = cur.find(0xA3);
            if (nk_it != cur.end()) {
                const auto& nk = nk_it->second;
                if (nk.stride == 3) {
                    if (nk.raw.size() < 3 * n) { truncated = true; cur.clear(); return; }
                    b.hasNormals = true;
                    b.normals.resize(3 * n);
                    for (uint32_t i = 0; i < n; ++i) {
                        b.normals[i*3] = (int32_t)(int8_t)nk.raw[3 * i];
                        b.normals[i*3+1] = (int32_t)(int8_t)nk.raw[3 * i + 1];
                        b.normals[i*3+2] = (int32_t)(int8_t)nk.raw[3 * i + 2];
                    }
                } else if (nk.stride == 4) {
                    if (nk.raw.size() < 4 * n) { truncated = true; cur.clear(); return; }
                    b.hasColours = true;
                    b.colours.resize(4 * n);
                    for (uint32_t i = 0; i < n; ++i) {
                        b.colours[i*4] = std::min(nk.raw[4 * i] / 128.0, 1.0);
                        b.colours[i*4+1] = std::min(nk.raw[4 * i + 1] / 128.0, 1.0);
                        b.colours[i*4+2] = std::min(nk.raw[4 * i + 2] / 128.0, 1.0);
                        b.colours[i*4+3] = std::min(nk.raw[4 * i + 3] / 128.0, 1.0);
                    }
                }
            }
            
            batches.push_back(b);
            cur.clear();
        } else if (cur.find(0x03) != cur.end() && cur[0x03].stride == 16) {
            const auto& pk = cur[0x03];
            uint32_t n = pk.count;
            uint8_t stride = pk.stride;
            
            SkinnedBatch b;
            b.positions.reserve(3 * n);
            if (pk.raw.size() < stride * n) { truncated = true; cur.clear(); return; }
            for (uint32_t i = 0; i < n; ++i) {
                float px, py, pz;
                if (!readF32(pk.raw.data(), pk.raw.size(), stride * i, px) ||
                    !readF32(pk.raw.data(), pk.raw.size(), stride * i + 4, py) ||
                    !readF32(pk.raw.data(), pk.raw.size(), stride * i + 8, pz)) { truncated = true; cur.clear(); return; }
                b.positions.push_back(px);
                b.positions.push_back(py);
                b.positions.push_back(pz);
            }
            
            b.uvs.assign(n * 2, 0);
            b.flags.assign(n, 0);
            auto uk_it = cur.find(0x43);
            if (uk_it != cur.end()) {
                const auto& uk = uk_it->second;
                uint32_t m = std::min(n, uk.count);
                if (uk.raw.size() < 6 * m) { truncated = true; cur.clear(); return; }
                for (uint32_t i = 0; i < m; ++i) {
                    int16_t a, b_uv, c;
                    if (!readS16(uk.raw.data(), uk.raw.size(), 6 * i, a) ||
                        !readS16(uk.raw.data(), uk.raw.size(), 6 * i + 2, b_uv) ||
                        !readS16(uk.raw.data(), uk.raw.size(), 6 * i + 4, c)) { truncated = true; cur.clear(); return; }
                    b.uvs[i * 2] = a;
                    b.uvs[i * 2 + 1] = b_uv;
                    b.flags[i] = c;
                }
            }
            
            b.bones.assign(n, {1, 0, 0});
            b.weights.assign(3 * n, 0.0f);
            for (uint32_t i = 0; i < n; ++i) b.weights[i*3] = 1.0f;
            
            b.hasNormals = false;
            b.hasColours = false;
            auto nk_it = cur.find(0x83);
            if (nk_it != cur.end()) {
                const auto& nk = nk_it->second;
                if (nk.stride == 3) {
                    if (nk.raw.size() < 3 * n) { truncated = true; cur.clear(); return; }
                    b.hasNormals = true;
                    b.normals.resize(3 * n);
                    for (uint32_t i = 0; i < n; ++i) {
                        b.normals[i*3] = (int32_t)(int8_t)nk.raw[3 * i];
                        b.normals[i*3+1] = (int32_t)(int8_t)nk.raw[3 * i + 1];
                        b.normals[i*3+2] = (int32_t)(int8_t)nk.raw[3 * i + 2];
                    }
                }
            }
            
            batches.push_back(b);
            cur.clear();
        } else {
            cur.clear();
        }
    };

    size_t p = o;
    size_t end = o + size;
    while (p + 16 <= end) {
        uint32_t lo;
        if (!readU32(d, len, p, lo)) return false;
        uint16_t qwc = lo & 0xFFFF;
        uint8_t tid = (lo >> 28) & 7;
        size_t stop = std::min(p + 16 + 16 * qwc, end);
        size_t v = p + 8;
        while (v + 4 <= stop) {
            uint32_t w;
            if (!readU32(d, len, v, w)) return false;
            uint8_t cmd = (w >> 24) & 0x7F;
            uint8_t num = (w >> 16) & 0xFF;
            uint16_t imm = w & 0xFFFF;
            if (cmd >= 0x60) {
                uint8_t f = cmd & 0xF;
                auto el_it = EL.find(f);
                if (el_it == EL.end()) return false;
                uint8_t el = el_it->second;
                uint32_t n = num ? num : 256;
                size_t nbytes = ((size_t)el * n + 3) / 4 * 4;
                uint32_t addr = imm & 0x3FF;
                if (cur.find(addr) != cur.end()) flush();
                size_t data_start = v + 4;
                size_t raw_len = 0;
                if (data_start < len) {
                    raw_len = std::min((size_t)el * n, len - data_start);
                }
                RawData rd;
                if (raw_len)
                    rd.raw.assign(d + data_start, d + data_start + raw_len);
                rd.count = n;
                rd.stride = el;
                cur[addr] = rd;
                v += 4 + nbytes;
            } else {
                if (cmd == 0x17) flush();
                size_t skip = 4;
                if (cmd == 0x20) skip += 4;
                else if (cmd == 0x30 || cmd == 0x31) skip += 16;
                if (cmd == 0x50 || cmd == 0x51) skip += 16 * (imm ? imm : 65536);
                v += skip;
            }
        }
        p += 16 + 16 * qwc;
        if (tid == 6 || tid == 7) break;
    }
    flush();
    if (truncated)
        return false;
    out = batches;
    return true;
}

bool batchMesh(const Batch &b, double uvscale, Mesh &out) {
    std::vector<float> pos;
    uint32_t n = 0;
    const uint8_t *raw = nullptr;
    
    auto it8 = b.find(8);
    if (it8 != b.end()) {
        n = it8->second.count;
        raw = it8->second.raw.data();
        pos.resize(3 * n);
        for (uint32_t i = 0; i < n; ++i) {
            float px, py, pz;
            if (!readF32(raw, it8->second.raw.size(), 12 * i, px) ||
                !readF32(raw, it8->second.raw.size(), 12 * i + 4, py) ||
                !readF32(raw, it8->second.raw.size(), 12 * i + 8, pz)) return false;
            pos[i*3] = px; pos[i*3+1] = py; pos[i*3+2] = pz;
        }
    } else {
        auto itC = b.find(0xC);
        if (itC != b.end() && itC->second.addr == 3) {
            n = itC->second.count;
            raw = itC->second.raw.data();
            pos.resize(3 * n);
            for (uint32_t i = 0; i < n; ++i) {
                float px, py, pz;
                if (!readF32(raw, itC->second.raw.size(), 16 * i, px) ||
                    !readF32(raw, itC->second.raw.size(), 16 * i + 4, py) ||
                    !readF32(raw, itC->second.raw.size(), 16 * i + 8, pz)) return false;
                pos[i*3] = px; pos[i*3+1] = py; pos[i*3+2] = pz;
            }
        } else {
            return false;
        }
    }
    
    out.positions = pos;
    
    std::vector<double> uv(2 * n, 0.0);
    std::vector<uint32_t> flags(n, 0);
    
    auto it9 = b.find(9);
    if (it9 != b.end()) {
        uint32_t m = it9->second.count;
        const uint8_t *r = it9->second.raw.data();
        uint32_t lim = std::min(n, m);
        if (it9->second.raw.size() < 6 * lim) return false;
        for (uint32_t i = 0; i < lim; ++i) {
            int16_t s, t, f;
            if (!readS16(r, it9->second.raw.size(), 6 * i, s) ||
                !readS16(r, it9->second.raw.size(), 6 * i + 2, t) ||
                !readS16(r, it9->second.raw.size(), 6 * i + 4, f)) return false;
            uv[i * 2] = s / uvscale;
            uv[i * 2 + 1] = t / uvscale;
            flags[i] = f;
        }
    }
    out.uvs = uv;
    
    std::vector<double> col(4 * n, 1.0);
    auto itE = b.find(0xE);
    if (itE != b.end()) {
        uint32_t m = itE->second.count;
        const uint8_t *r = itE->second.raw.data();
        uint32_t lim = std::min(n, m);
        if (itE->second.raw.size() < 4 * lim) return false;
        for (uint32_t i = 0; i < lim; ++i) {
            col[i * 4] = std::min(r[4 * i] / 128.0, 1.0);
            col[i * 4 + 1] = std::min(r[4 * i + 1] / 128.0, 1.0);
            col[i * 4 + 2] = std::min(r[4 * i + 2] / 128.0, 1.0);
            col[i * 4 + 3] = std::min(r[4 * i + 3] / 128.0, 1.0);
        }
    }
    out.colours = col;
    
    out.tris.clear();
    for (uint32_t i = 2; i < n; ++i) {
        if (flags[i]) continue;
        if (i % 2 == 0) {
            out.tris.push_back(i - 2);
            out.tris.push_back(i - 1);
            out.tris.push_back(i);
        } else {
            out.tris.push_back(i - 1);
            out.tris.push_back(i - 2);
            out.tris.push_back(i);
        }
    }
    return true;
}

} // namespace pop_mesh
