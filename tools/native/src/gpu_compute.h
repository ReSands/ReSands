// Minimal Vulkan compute runner: storage buffers in host-visible memory, one compute pipeline per
// SPIR-V module (bindings 0..N-1 = storage buffers, push constants up to 128 bytes), synchronous
// dispatches. Used to judge GPU ports of the post effects (postfx.h) against their CPU references,
// and as the first building block of the GPU post-processing chain. Portable (volk; MoltenVK on macOS).
#pragma once
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace pop_gpu
{
    class Compute
    {
    public:
        Compute();
        ~Compute();
        bool init(std::string &error);
        std::string deviceName() const;

        // a storage buffer of `bytes` bytes, mapped (contents readable/writable through data())
        int createBuffer(size_t bytes);
        void *data(int buffer);
        size_t size(int buffer) const;

        // a compute pipeline from SPIR-V words; `bindings` storage buffers at set 0, bindings 0..n-1
        int createPipeline(const std::vector<uint32_t> &spirv, int bindings, std::string &error);

        // runs pipeline with the given buffers and push constants, groups x/y/z, and waits for it
        bool dispatch(int pipeline, const std::vector<int> &buffers, const void *push, size_t pushBytes, uint32_t gx, uint32_t gy,
                      uint32_t gz, std::string &error);

    private:
        struct Impl;
        std::unique_ptr<Impl> m;
    };

    // reads a SPIR-V file (as written by glslangValidator -V)
    bool readSpirv(const std::string &path, std::vector<uint32_t> &words);
}
