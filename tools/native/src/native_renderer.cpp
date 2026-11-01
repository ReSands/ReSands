// Native renderer core: see native_renderer.h.
#include "native_renderer.h"
#include "postfx.h"
#include "shaders_spv.h"

#include <volk.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <unordered_map>
#if defined(__APPLE__)
#include <dlfcn.h>
#endif

namespace pop_native
{
    namespace
    {
        struct Buffer
        {
            VkBuffer buffer = VK_NULL_HANDLE;
            VkDeviceMemory memory = VK_NULL_HANDLE;
            VkDeviceSize size = 0;
        };
        struct Texture
        {
            VkImage image = VK_NULL_HANDLE;
            VkDeviceMemory memory = VK_NULL_HANDLE;
            VkImageView view = VK_NULL_HANDLE;
            uint32_t width = 0, height = 0;
        };
        struct MeshRange
        {
            uint32_t firstIndex, indexCount;
            int32_t vertexOffset;
            int material;
        };
        struct Push
        {
            float mvp[16];
            float params[4];
            float fog[4];
            float fogColour[4];
        };

        bool check(VkResult r, const char *what, std::string &error)
        {
            if (r == VK_SUCCESS)
                return true;
            error = std::string(what) + " failed (VkResult " + std::to_string(static_cast<int>(r)) + ")";
            return false;
        }

        // column-major 4x4 multiply: out = a * b
        void mul(const float a[16], const float b[16], float out[16])
        {
            for (int c = 0; c < 4; ++c)
                for (int r = 0; r < 4; ++r)
                {
                    float s = 0;
                    for (int k = 0; k < 4; ++k)
                        s += a[k * 4 + r] * b[c * 4 + k];
                    out[c * 4 + r] = s;
                }
        }
    }

    struct Renderer::Impl
    {
        Options options;
        Fog fog;
        VkInstance instance = VK_NULL_HANDLE;
        VkPhysicalDevice physical = VK_NULL_HANDLE;
        VkPhysicalDeviceProperties props{};
        VkDevice device = VK_NULL_HANDLE;
        uint32_t queueFamily = 0;
        VkQueue queue = VK_NULL_HANDLE;
        VkCommandPool commandPool = VK_NULL_HANDLE;
        VkSampler sampler = VK_NULL_HANDLE;
        VkSampler clampSampler = VK_NULL_HANDLE; // Material::clampUv (shadow receivers: GS CLAMP 5)
        VkDescriptorSetLayout setLayout = VK_NULL_HANDLE;
        VkPipelineLayout pipelineLayout = VK_NULL_HANDLE;
        VkDescriptorPool descriptorPool = VK_NULL_HANDLE;
        VkRenderPass renderPass = VK_NULL_HANDLE;
        VkRenderPass renderPassLoad = VK_NULL_HANDLE; // the scene pass resumed after a mid-frame copy (Draw::copyFrame)
        Texture frameCopy;                            // kFrameCopyImage: the frame at the last copyFrame
        Texture frameSave;                            // kFrameSaveImage: the frame at the last saveFrame
        uint32_t frameCopyW = 0, frameCopyH = 0;
        Buffer vertexBuffer, indexBuffer;
        // kept meshes (keepMesh): append-only device buffers, grown by doubling; pending = not uploaded yet
        struct KeptRange
        {
            uint32_t firstIndex, indexCount;
            int32_t vertexOffset;
        };
        Buffer keptVertices, keptIndices;
        VkDeviceSize keptVertexCap = 0, keptIndexCap = 0; // in elements
        uint32_t keptVertexCount = 0, keptIndexCount = 0;  // uploaded
        std::vector<KeptRange> kept;
        std::unordered_map<uint64_t, int> keptOf;
        std::vector<Vertex> keptPendingVertices;
        std::vector<uint32_t> keptPendingIndices;
        uint32_t keptEpoch = 0;

        std::vector<Image> images;
        std::vector<std::pair<int, Image>> imageUpdates; // updateImage: (image index, new pixels) for uploaded images
        std::vector<Material> materials;
        std::vector<Mesh> meshes;
        std::vector<MeshRange> ranges;
        std::vector<Texture> textures; // images, then white
        std::vector<VkDescriptorSet> materialSets;
        VkDescriptorSet whiteSet = VK_NULL_HANDLE;

        // windowed presentation
        VkSurfaceKHR surface = VK_NULL_HANDLE;
        VkSwapchainKHR swapchain = VK_NULL_HANDLE;
        VkFormat swapFormat = VK_FORMAT_UNDEFINED;
        VkExtent2D swapExtent{};
        std::vector<VkImage> swapImages;
        // renderOffscreen's targets, kept between calls (the in-game native world draws every frame);
        // the readback buffers are host-cached where the device has such memory: reading uncached
        // (write-combined) memory cost ~30 ms per 640x448 frame on an RTX 5070 Ti under Windows
        struct Offscreen
        {
            uint32_t w = 0, h = 0;
            Texture colour, depth;
            Buffer readback, depthReadback;
            VkFramebuffer fb = VK_NULL_HANDLE;
            bool cached = false; // readback memory is HOST_CACHED (then not necessarily coherent: invalidate)
            void *colourMap = nullptr, *depthMap = nullptr;
        } off;
        Texture targetColour, targetDepth; // the frame at internal resolution, blitted to the window
        VkFramebuffer targetFb = VK_NULL_HANDLE;
        uint32_t targetW = 0, targetH = 0;
        VkCommandBuffer frameCb = VK_NULL_HANDLE;
        VkFence frameFence = VK_NULL_HANDLE;
        VkSemaphore acquired = VK_NULL_HANDLE, rendered = VK_NULL_HANDLE;

        uint32_t memoryType(uint32_t bits, VkMemoryPropertyFlags flags) const
        {
            VkPhysicalDeviceMemoryProperties mp;
            vkGetPhysicalDeviceMemoryProperties(physical, &mp);
            for (uint32_t i = 0; i < mp.memoryTypeCount; ++i)
                if ((bits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & flags) == flags)
                    return i;
            return UINT32_MAX;
        }

        bool createBuffer(VkDeviceSize size, VkBufferUsageFlags usage, VkMemoryPropertyFlags flags, Buffer &out, std::string &error)
        {
            VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
            bi.size = size ? size : 16;
            bi.usage = usage;
            if (!check(vkCreateBuffer(device, &bi, nullptr, &out.buffer), "vkCreateBuffer", error))
                return false;
            VkMemoryRequirements req;
            vkGetBufferMemoryRequirements(device, out.buffer, &req);
            VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
            ai.allocationSize = req.size;
            ai.memoryTypeIndex = memoryType(req.memoryTypeBits, flags);
            if (ai.memoryTypeIndex == UINT32_MAX)
            {
                error = "no suitable memory type for a buffer";
                return false;
            }
            if (!check(vkAllocateMemory(device, &ai, nullptr, &out.memory), "vkAllocateMemory", error))
                return false;
            out.size = bi.size;
            return check(vkBindBufferMemory(device, out.buffer, out.memory, 0), "vkBindBufferMemory", error);
        }

        void destroyBuffer(Buffer &b)
        {
            if (b.buffer)
                vkDestroyBuffer(device, b.buffer, nullptr);
            if (b.memory)
                vkFreeMemory(device, b.memory, nullptr);
            b = {};
        }

        bool createImage(uint32_t w, uint32_t h, VkFormat format, VkImageUsageFlags usage, VkImageAspectFlags aspect,
                         Texture &out, std::string &error)
        {
            VkImageCreateInfo ii{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
            ii.imageType = VK_IMAGE_TYPE_2D;
            ii.format = format;
            ii.extent = {w, h, 1};
            ii.mipLevels = 1;
            ii.arrayLayers = 1;
            ii.samples = VK_SAMPLE_COUNT_1_BIT;
            ii.tiling = VK_IMAGE_TILING_OPTIMAL;
            ii.usage = usage;
            ii.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            if (!check(vkCreateImage(device, &ii, nullptr, &out.image), "vkCreateImage", error))
                return false;
            VkMemoryRequirements req;
            vkGetImageMemoryRequirements(device, out.image, &req);
            VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
            ai.allocationSize = req.size;
            ai.memoryTypeIndex = memoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
            if (!check(vkAllocateMemory(device, &ai, nullptr, &out.memory), "vkAllocateMemory (image)", error) ||
                !check(vkBindImageMemory(device, out.image, out.memory, 0), "vkBindImageMemory", error))
                return false;
            VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
            vi.image = out.image;
            vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
            vi.format = format;
            vi.subresourceRange = {aspect, 0, 1, 0, 1};
            return check(vkCreateImageView(device, &vi, nullptr, &out.view), "vkCreateImageView", error);
        }

        void destroyImage(Texture &t)
        {
            if (t.view)
                vkDestroyImageView(device, t.view, nullptr);
            if (t.image)
                vkDestroyImage(device, t.image, nullptr);
            if (t.memory)
                vkFreeMemory(device, t.memory, nullptr);
            t = {};
        }

        // one-shot command buffer, submitted and waited for
        template <typename F>
        bool oneShot(F &&record, std::string &error)
        {
            VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
            ai.commandPool = commandPool;
            ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
            ai.commandBufferCount = 1;
            VkCommandBuffer cb;
            if (!check(vkAllocateCommandBuffers(device, &ai, &cb), "vkAllocateCommandBuffers", error))
                return false;
            VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
            bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
            vkBeginCommandBuffer(cb, &bi);
            record(cb);
            vkEndCommandBuffer(cb);
            VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
            si.commandBufferCount = 1;
            si.pCommandBuffers = &cb;
            const bool ok = check(vkQueueSubmit(queue, 1, &si, VK_NULL_HANDLE), "vkQueueSubmit", error) &&
                            check(vkQueueWaitIdle(queue), "vkQueueWaitIdle", error);
            vkFreeCommandBuffers(device, commandPool, 1, &cb);
            return ok;
        }

        static void barrier(VkCommandBuffer cb, VkImage image, VkImageLayout from, VkImageLayout to, VkAccessFlags srcAccess,
                            VkAccessFlags dstAccess, VkPipelineStageFlags srcStage, VkPipelineStageFlags dstStage,
                            VkImageAspectFlags aspect = VK_IMAGE_ASPECT_COLOR_BIT)
        {
            VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
            b.oldLayout = from;
            b.newLayout = to;
            b.srcAccessMask = srcAccess;
            b.dstAccessMask = dstAccess;
            b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            b.image = image;
            b.subresourceRange = {aspect, 0, 1, 0, 1};
            vkCmdPipelineBarrier(cb, srcStage, dstStage, 0, 0, nullptr, 0, nullptr, 1, &b);
        }

        bool uploadTexture(const Image &img, Texture &out, std::string &error)
        {
            if (!createImage(img.width, img.height, VK_FORMAT_R8G8B8A8_UNORM,
                             VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, VK_IMAGE_ASPECT_COLOR_BIT, out, error))
                return false;
            Buffer staging;
            if (!createBuffer(img.rgba.size(), VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                              VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, staging, error))
                return false;
            void *p;
            vkMapMemory(device, staging.memory, 0, img.rgba.size(), 0, &p);
            std::memcpy(p, img.rgba.data(), img.rgba.size());
            vkUnmapMemory(device, staging.memory);
            const bool ok = oneShot([&](VkCommandBuffer cb) {
                barrier(cb, out.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0,
                        VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
                VkBufferImageCopy region{};
                region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
                region.imageExtent = {img.width, img.height, 1};
                vkCmdCopyBufferToImage(cb, staging.buffer, out.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
                barrier(cb, out.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                        VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                        VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
            }, error);
            destroyBuffer(staging);
            return ok;
        }

        // several textures with one command buffer (one queue wait instead of one per texture)
        bool uploadTextures(const std::vector<Image> &imgs, Texture *out, std::string &error)
        {
            VkDeviceSize total = 0;
            for (size_t i = 0; i < imgs.size(); ++i)
            {
                if (!createImage(imgs[i].width, imgs[i].height, VK_FORMAT_R8G8B8A8_UNORM,
                                 VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, VK_IMAGE_ASPECT_COLOR_BIT, out[i], error))
                    return false;
                out[i].width = imgs[i].width;
                out[i].height = imgs[i].height;
                total += (imgs[i].rgba.size() + 15) & ~VkDeviceSize(15);
            }
            Buffer staging;
            if (!createBuffer(total, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                              staging, error))
                return false;
            uint8_t *p = nullptr;
            vkMapMemory(device, staging.memory, 0, total ? total : 16, 0, reinterpret_cast<void **>(&p));
            std::vector<VkDeviceSize> offsets(imgs.size());
            VkDeviceSize at = 0;
            for (size_t i = 0; i < imgs.size(); ++i)
            {
                offsets[i] = at;
                std::memcpy(p + at, imgs[i].rgba.data(), imgs[i].rgba.size());
                at += (imgs[i].rgba.size() + 15) & ~VkDeviceSize(15);
            }
            vkUnmapMemory(device, staging.memory);
            const bool ok = oneShot([&](VkCommandBuffer cb) {
                for (size_t i = 0; i < imgs.size(); ++i)
                {
                    barrier(cb, out[i].image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT,
                            VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
                    VkBufferImageCopy region{};
                    region.bufferOffset = offsets[i];
                    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
                    region.imageExtent = {imgs[i].width, imgs[i].height, 1};
                    vkCmdCopyBufferToImage(cb, staging.buffer, out[i].image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
                    barrier(cb, out[i].image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                            VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                            VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
                }
            }, error);
            destroyBuffer(staging);
            return ok;
        }

        // new pixels for textures already on the GPU (same size): after the frames that sampled them (queue order)
        bool rewriteTextures(std::string &error)
        {
            VkDeviceSize total = 0;
            for (const auto &u : imageUpdates)
                total += (u.second.rgba.size() + 15) & ~VkDeviceSize(15);
            if (!total)
            {
                imageUpdates.clear();
                return true;
            }
            Buffer staging;
            if (!createBuffer(total, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                              staging, error))
                return false;
            uint8_t *p = nullptr;
            vkMapMemory(device, staging.memory, 0, total, 0, reinterpret_cast<void **>(&p));
            VkDeviceSize at = 0;
            std::vector<VkDeviceSize> offsets;
            for (const auto &u : imageUpdates)
            {
                offsets.push_back(at);
                std::memcpy(p + at, u.second.rgba.data(), u.second.rgba.size());
                at += (u.second.rgba.size() + 15) & ~VkDeviceSize(15);
            }
            vkUnmapMemory(device, staging.memory);
            const bool ok = oneShot([&](VkCommandBuffer cb) {
                for (size_t i = 0; i < imageUpdates.size(); ++i)
                {
                    const Texture &t = textures[size_t(imageUpdates[i].first) + 1];
                    barrier(cb, t.image, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_ACCESS_SHADER_READ_BIT,
                            VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
                    VkBufferImageCopy region{};
                    region.bufferOffset = offsets[i];
                    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
                    region.imageExtent = {t.width, t.height, 1};
                    vkCmdCopyBufferToImage(cb, staging.buffer, t.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
                    barrier(cb, t.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT,
                            VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
                }
            }, error);
            destroyBuffer(staging);
            imageUpdates.clear();
            return ok;
        }

        bool uploadBuffer(const void *data, VkDeviceSize size, VkBufferUsageFlags usage, Buffer &out, std::string &error)
        {
            if (!createBuffer(size, usage | VK_BUFFER_USAGE_TRANSFER_DST_BIT, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, out, error))
                return false;
            if (size == 0) // a frame of kept meshes only: the buffer stays empty (a zero-size map or copy is invalid)
                return true;
            Buffer staging;
            if (!createBuffer(size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                              VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, staging, error))
                return false;
            void *p;
            vkMapMemory(device, staging.memory, 0, size, 0, &p);
            std::memcpy(p, data, size);
            vkUnmapMemory(device, staging.memory);
            const bool ok = oneShot([&](VkCommandBuffer cb) {
                VkBufferCopy c{0, 0, size};
                vkCmdCopyBuffer(cb, staging.buffer, out.buffer, 1, &c);
            }, error);
            destroyBuffer(staging);
            return ok;
        }

        void dropKept()
        {
            destroyBuffer(keptVertices);
            destroyBuffer(keptIndices);
            keptVertexCap = keptIndexCap = 0;
            keptVertexCount = keptIndexCount = 0;
            kept.clear();
            keptOf.clear();
            keptPendingVertices.clear();
            keptPendingIndices.clear();
            ++keptEpoch;
        }

        // appends the pending kept meshes to the kept buffers (doubling them, old contents copied on the GPU)
        bool uploadKept(size_t budget, std::string &error)
        {
            if (keptPendingIndices.empty() && keptPendingVertices.empty())
                return true;
            const size_t nv = keptVertexCount + keptPendingVertices.size(), ni = keptIndexCount + keptPendingIndices.size();
            if (nv * sizeof(Vertex) + ni * 4 > budget)
            {
                std::fprintf(stderr, "[native] kept meshes over budget (%zu MB): dropped, epoch %u\n", budget >> 20, keptEpoch + 1);
                dropKept();
                return true; // this frame's draws of kept meshes are skipped; the sender sends them again
            }
            const auto grow = [&](Buffer &buf, VkDeviceSize &cap, size_t need, size_t used, size_t elem, VkBufferUsageFlags usage) -> bool {
                if (need <= cap)
                    return true;
                VkDeviceSize newCap = cap ? cap : 65536;
                while (newCap < need)
                    newCap *= 2;
                Buffer bigger;
                if (!createBuffer(newCap * elem, usage | VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                                  VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, bigger, error))
                    return false;
                if (used && !oneShot([&](VkCommandBuffer cb) {
                        VkBufferCopy c{0, 0, used * elem};
                        vkCmdCopyBuffer(cb, buf.buffer, bigger.buffer, 1, &c);
                    }, error))
                    return false;
                destroyBuffer(buf);
                buf = bigger;
                cap = newCap;
                return true;
            };
            if (!grow(keptVertices, keptVertexCap, nv, keptVertexCount, sizeof(Vertex), VK_BUFFER_USAGE_VERTEX_BUFFER_BIT) ||
                !grow(keptIndices, keptIndexCap, ni, keptIndexCount, 4, VK_BUFFER_USAGE_INDEX_BUFFER_BIT))
                return false;
            const VkDeviceSize vBytes = keptPendingVertices.size() * sizeof(Vertex), iBytes = keptPendingIndices.size() * 4;
            Buffer staging;
            if (!createBuffer(vBytes + iBytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                              VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, staging, error))
                return false;
            void *p;
            vkMapMemory(device, staging.memory, 0, vBytes + iBytes, 0, &p);
            std::memcpy(p, keptPendingVertices.data(), vBytes);
            std::memcpy(static_cast<uint8_t *>(p) + vBytes, keptPendingIndices.data(), iBytes);
            vkUnmapMemory(device, staging.memory);
            const bool ok = oneShot([&](VkCommandBuffer cb) {
                if (vBytes)
                {
                    VkBufferCopy c{0, keptVertexCount * sizeof(Vertex), vBytes};
                    vkCmdCopyBuffer(cb, staging.buffer, keptVertices.buffer, 1, &c);
                }
                if (iBytes)
                {
                    VkBufferCopy c{vBytes, keptIndexCount * VkDeviceSize(4), iBytes};
                    vkCmdCopyBuffer(cb, staging.buffer, keptIndices.buffer, 1, &c);
                }
            }, error);
            destroyBuffer(staging);
            keptVertexCount = uint32_t(nv);
            keptIndexCount = uint32_t(ni);
            keptPendingVertices.clear();
            keptPendingIndices.clear();
            return ok;
        }

        VkShaderModule shader(const uint32_t *code, size_t bytes, std::string &error)
        {
            VkShaderModuleCreateInfo si{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
            si.codeSize = bytes;
            si.pCode = code;
            VkShaderModule m = VK_NULL_HANDLE;
            check(vkCreateShaderModule(device, &si, nullptr, &m), "vkCreateShaderModule", error);
            return m;
        }

        // the frame copy (kFrameCopyImage) at the frame's size, its materials' descriptors pointing at it; only when a
        // draw asks for it. Call with no frame in flight (descriptor sets are rewritten).
        bool prepareFrameCopy(const std::vector<Draw> &draws, uint32_t w, uint32_t h, std::string &error)
        {
            const bool copies = std::any_of(draws.begin(), draws.end(), [](const Draw &d) { return d.copyFrame || d.saveFrame; });
            const bool samples = std::any_of(materials.begin(), materials.end(),
                                             [](const Material &mt) { return mt.baseImage == kFrameCopyImage || mt.baseImage == kFrameSaveImage; });
            if (!copies && !samples)
                return true;
            if (!frameCopy.image || frameCopyW != w || frameCopyH != h)
            {
                vkDeviceWaitIdle(device);
                destroyImage(frameCopy);
                destroyImage(frameSave);
                frameCopyW = frameCopyH = 0;
                for (Texture *t : {&frameCopy, &frameSave})
                    if (!createImage(w, h, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, VK_IMAGE_ASPECT_COLOR_BIT,
                                     *t, error))
                        return false;
                if (!oneShot([&](VkCommandBuffer cb) {
                        for (Texture *t : {&frameCopy, &frameSave})
                            barrier(cb, t->image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, 0, VK_ACCESS_SHADER_READ_BIT,
                                    VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
                    }, error))
                    return false;
                frameCopyW = w;
                frameCopyH = h;
            }
            for (size_t i = 0; i < materials.size() && i < materialSets.size(); ++i)
                if (materials[i].baseImage == kFrameCopyImage || materials[i].baseImage == kFrameSaveImage)
                {
                    const VkDescriptorImageInfo info{clampSampler, (materials[i].baseImage == kFrameSaveImage ? frameSave : frameCopy).view,
                                                     VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
                    VkWriteDescriptorSet wr{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
                    wr.dstSet = materialSets[i];
                    wr.dstBinding = 0;
                    wr.descriptorCount = 1;
                    wr.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
                    wr.pImageInfo = &info;
                    vkUpdateDescriptorSets(device, 1, &wr, 0, nullptr);
                }
            return true;
        }

        // the scene pass into `fb` (colour ends in TRANSFER_SRC layout, see the render pass); `colour` is fb's colour
        // image, copied into frameCopy at each Draw::copyFrame (prepareFrameCopy first)
        // twoD: the 2D layer (Draw::afterPost), resumed over the scene after the post effects, without a clear
        void recordScene(VkCommandBuffer cb, VkFramebuffer fb, VkImage colour, uint32_t width, uint32_t height, const float clip[16],
                         const std::vector<Draw> &draws, bool twoD = false)
        {
            // The game's clip matrix is stored for row vectors (clip = [x y z 1] * C), so read as a
            // column-major array it is already C transposed, the matrix for column vectors.
            VkClearValue clear[2];
            std::memcpy(clear[0].color.float32, options.clearColour, sizeof(float) * 4);
            clear[1].depthStencil = {0.0f, 0};
            VkRenderPassBeginInfo rb{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
            rb.renderPass = twoD ? renderPassLoad : renderPass;
            rb.framebuffer = fb;
            rb.renderArea.extent = {width, height};
            rb.clearValueCount = twoD ? 0 : 2;
            rb.pClearValues = twoD ? nullptr : clear;
            vkCmdBeginRenderPass(cb, &rb, VK_SUBPASS_CONTENTS_INLINE);
            // the GS samples a pixel at its integer coordinate, Vulkan at its centre: shift the picture by
            // half a GS pixel (640x448 across the target) so both sample the same points (Z then matches
            // the game's to ~5e-5, 19 Oct)
            VkViewport vp{0.5f * float(width) / 640.0f, 0.5f * float(height) / 448.0f, float(width), float(height), 0.0f, 1.0f};
            VkRect2D sc{{0, 0}, {width, height}};
            vkCmdSetViewport(cb, 0, 1, &vp);
            vkCmdSetScissor(cb, 0, 1, &sc);
            VkDeviceSize zero = 0;
            int boundBuffers = -1; // 0: this frame's meshes, 1: the kept meshes
            // opaque + alpha-tested, then blended in draw order; or one pass in the given order
            for (int pass = 0; pass < (options.gameOrder ? 1 : 2); ++pass)
            {
                VkPipeline bound = VK_NULL_HANDLE;
                for (const Draw &d : draws)
                {
                    if (d.afterPost != twoD)
                        continue;
                    if ((d.copyFrame || d.saveFrame) && pass == 0 && frameCopy.image && frameCopyW == width && frameCopyH == height)
                    {
                        const VkImage target = d.saveFrame ? frameSave.image : frameCopy.image;
                        // the frame so far (colour + alpha) into frameCopy, then the pass goes on over it
                        vkCmdEndRenderPass(cb);
                        barrier(cb, target, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_ACCESS_SHADER_READ_BIT,
                                VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
                        VkImageCopy c{};
                        c.srcSubresource = c.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
                        const auto px = [](float f, uint32_t size) { return uint32_t(std::clamp(f, 0.0f, 1.0f) * float(size)); };
                        const uint32_t x0 = px(d.copyRect[0], width), y0 = px(d.copyRect[1], height);
                        const uint32_t x1 = std::max(px(d.copyRect[2], width), x0), y1 = std::max(px(d.copyRect[3], height), y0);
                        c.srcOffset = c.dstOffset = {int32_t(x0), int32_t(y0), 0};
                        c.extent = {x1 - x0, y1 - y0, 1};
                        if (c.extent.width && c.extent.height) // Vulkan forbids an empty copy (a rect off the frame)
                            vkCmdCopyImage(cb, colour, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, target, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &c);
                        barrier(cb, target, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT,
                                VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
                        VkRenderPassBeginInfo again = rb;
                        again.renderPass = renderPassLoad;
                        again.clearValueCount = 0;
                        again.pClearValues = nullptr;
                        vkCmdBeginRenderPass(cb, &again, VK_SUBPASS_CONTENTS_INLINE);
                        vkCmdSetViewport(cb, 0, 1, &vp);
                        vkCmdSetScissor(cb, 0, 1, &sc);
                        bound = VK_NULL_HANDLE;
                        boundBuffers = -1;
                    }
                    MeshRange r;
                    const bool isKept = d.mesh < 0 && d.kept >= 0;
                    if (isKept)
                    {
                        if (static_cast<size_t>(d.kept) >= kept.size() || !keptVertices.buffer)
                            continue;
                        const KeptRange &k = kept[d.kept];
                        r = {k.firstIndex, k.indexCount, k.vertexOffset, d.material};
                    }
                    else if (d.mesh < 0 || static_cast<size_t>(d.mesh) >= ranges.size())
                        continue;
                    else
                        r = ranges[d.mesh];
                    if (boundBuffers != int(isKept))
                    {
                        boundBuffers = int(isKept);
                        vkCmdBindVertexBuffers(cb, 0, 1, isKept ? &keptVertices.buffer : &vertexBuffer.buffer, &zero);
                        vkCmdBindIndexBuffer(cb, isKept ? keptIndices.buffer : indexBuffer.buffer, 0, VK_INDEX_TYPE_UINT32);
                    }
                    const Material *mat = r.material >= 0 && static_cast<size_t>(r.material) < materials.size() ? &materials[r.material] : nullptr;
                    const AlphaMode mode = mat ? mat->alphaMode : AlphaMode::Opaque;
                    if ((!options.gameOrder && isBlended(mode) != (pass == 1)) || r.indexCount == 0)
                        continue;
                    // AFAIL RGB_ONLY: second pass over the alpha-test failures, colour only
                    for (int afail = 0; afail < (mat && mat->afailRgbOnly ? 2 : 1); ++afail)
                    {
                    Material failMat;
                    if (afail)
                    {
                        failMat = *mat;
                        failMat.depthWrite = false;
                        failMat.writeAlpha = false;
                    }
                    std::string perr;
                    const VkPipeline want = pipeline(pipelineKey(afail ? &failMat : mat), perr);
                    if (!want)
                        continue;
                    if (want != bound)
                    {
                        vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, want);
                        bound = want;
                    }
                    Push push;
                    if (d.screen)
                        std::memcpy(push.mvp, d.model, sizeof push.mvp);
                    else
                        mul(clip, d.model, push.mvp);
                    // x: bit 0 alpha test, bit 1 blended (alpha out = texel * vertex alpha), bit 2 test LESS,
                    // bit 3 legacy blend (glTF BLEND: drop nearly transparent texels), bit 4 keep the
                    // failures instead (the failure pass keeps exactly the pixels the test rejects), bit 5 fog as vertex alpha
                    // (bit 6 no depth write, bit 7 the water's refraction read, bit 8 a halo's visibility: below, bit 9 AddAlpha: Cs * As, bit 10 TCC 0)
                    const bool test = mat && (mat->alphaTest || mode == AlphaMode::Mask);
                    const bool less = mat && mat->alphaTestLess;
                    // bit 6: no depth write (a decal over its wall): moved nearer by the decal bias (mesh.vert, N16 moss); not the
                    // lightmaps (fogAlpha): they redraw their base's own triangles, and the bias showed the part of a block's lightmap
                    // just under the ground as a dark seam along its foot (N18); nor the shadow receivers (clampUv), the same
                    // redraw of their base: biased, the Prince's shadow on the floor hidden under a block flickered at its foot
                    const bool noDepthWrite = !(pipelineKey(afail ? &failMat : mat) & (1u << 6)) && !(mat && (mat->fogAlpha || mat->clampUv));
                    push.params[0] = float((test ? 1 : 0) | (isBlended(mode) ? 2 : 0) | (less ? 4 : 0) | (mat && mat->legacyBlend ? 8 : 0) | (afail ? 16 : 0) | (mat && mat->fogAlpha ? 32 : 0) |
                                           (noDepthWrite ? 64 : 0) | (mat && mat->refraction ? 128 : 0) |
                                           (mat && mat->haloAlpha ? 256 : 0) | (mode == AlphaMode::AddAlpha && !mat->destAlpha ? 512 : 0) |
                                           (mat && mat->rgbTexture ? 1024 : 0));
                    push.params[1] = mat ? mat->cutoff : 0.5f;
                    push.params[2] = mat && mat->lightmapImage >= 0 ? mat->lightmapStrength : 0.0f;
                    push.params[3] = options.nearW / kGsZNear; // zScale (mesh.vert)
                    // no fog: F = 255 everywhere
                    const bool fogged = fog.enabled || d.ownFog;
                    push.fog[0] = !fogged ? 255.0f : d.ownFog ? d.fogA : fog.a;
                    push.fog[1] = !fogged ? 0.0f : d.ownFog ? d.fogB : fog.b;
                    push.fog[2] = !fogged ? 0.0f : d.ownFog ? d.fogMin : fog.min;
                    push.fog[3] = !fogged ? 255.0f : d.ownFog ? d.fogMax : fog.max;
                    const float *fc = d.ownFogColour ? d.fogColour : fog.colour;
                    push.fogColour[0] = fc[0];
                    push.fogColour[1] = fc[1];
                    push.fogColour[2] = fc[2];
                    // POP_NATIVE_DECAL_ZBIAS (GS Z units, default 32: 1 still flickered, 32 not, user 29 Oct): bit 64 of params.x moves draws without depth write nearer
                    static const float decalBias = [] {
                        const char *e = std::getenv("POP_NATIVE_DECAL_ZBIAS");
                        return e && *e ? std::strtof(e, nullptr) : 32.0f;
                    }();
                    push.fogColour[3] = decalBias;
                    VkDescriptorSet set = mat ? materialSets[r.material] : whiteSet;
                    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLayout, 0, 1, &set, 0, nullptr);
                    vkCmdPushConstants(cb, pipelineLayout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(Push), &push);
                    vkCmdDrawIndexed(cb, r.indexCount, 1, r.firstIndex, r.vertexOffset, 0);
                    }
                }
            }
            vkCmdEndRenderPass(cb);
        }

        bool ensureTarget(uint32_t w, uint32_t h, std::string &error)
        {
            if (targetFb && targetW == w && targetH == h)
                return true;
            vkDeviceWaitIdle(device);
            if (targetFb)
                vkDestroyFramebuffer(device, targetFb, nullptr);
            destroyImage(targetColour);
            destroyImage(targetDepth);
            targetFb = VK_NULL_HANDLE;
            // TRANSFER_DST: the GPU post effects copy the processed frame back into it
            if (!createImage(w, h, VK_FORMAT_R8G8B8A8_UNORM,
                             VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                             VK_IMAGE_ASPECT_COLOR_BIT, targetColour, error) ||
                !createImage(w, h, VK_FORMAT_D32_SFLOAT, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT, VK_IMAGE_ASPECT_DEPTH_BIT,
                             targetDepth, error))
                return false;
            VkImageView views[2] = {targetColour.view, targetDepth.view};
            VkFramebufferCreateInfo fi{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
            fi.renderPass = renderPass;
            fi.attachmentCount = 2;
            fi.pAttachments = views;
            fi.width = w;
            fi.height = h;
            fi.layers = 1;
            targetW = w;
            targetH = h;
            return check(vkCreateFramebuffer(device, &fi, nullptr, &targetFb), "vkCreateFramebuffer (target)", error);
        }

        bool ensureSwapchain(uint32_t w, uint32_t h, std::string &error)
        {
            if (swapchain && swapExtent.width == w && swapExtent.height == h)
                return true;
            vkDeviceWaitIdle(device);
            VkSurfaceCapabilitiesKHR caps;
            if (!check(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physical, surface, &caps), "surface capabilities", error))
                return false;
            uint32_t n = 0;
            vkGetPhysicalDeviceSurfaceFormatsKHR(physical, surface, &n, nullptr);
            std::vector<VkSurfaceFormatKHR> formats(n);
            vkGetPhysicalDeviceSurfaceFormatsKHR(physical, surface, &n, formats.data());
            // a UNORM (not sRGB) format: the game's colours are display values, as the PS2 outputs them
            VkSurfaceFormatKHR fmt = formats.empty() ? VkSurfaceFormatKHR{VK_FORMAT_B8G8R8A8_UNORM, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR} : formats[0];
            for (const auto &f : formats)
                if (f.format == VK_FORMAT_B8G8R8A8_UNORM || f.format == VK_FORMAT_R8G8B8A8_UNORM)
                {
                    fmt = f;
                    break;
                }
            VkExtent2D extent = caps.currentExtent.width != UINT32_MAX ? caps.currentExtent : VkExtent2D{w, h};
            if (extent.width == 0 || extent.height == 0)
                return true; // minimised
            VkSwapchainCreateInfoKHR si{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
            si.surface = surface;
            si.minImageCount = caps.maxImageCount ? std::min(caps.minImageCount + 1, caps.maxImageCount) : caps.minImageCount + 1;
            si.imageFormat = fmt.format;
            si.imageColorSpace = fmt.colorSpace;
            si.imageExtent = extent;
            si.imageArrayLayers = 1;
            si.imageUsage = VK_IMAGE_USAGE_TRANSFER_DST_BIT;
            si.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
            si.preTransform = caps.currentTransform;
            si.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
            si.presentMode = VK_PRESENT_MODE_FIFO_KHR; // always available; vsync
            si.clipped = VK_TRUE;
            si.oldSwapchain = swapchain;
            VkSwapchainKHR next;
            if (!check(vkCreateSwapchainKHR(device, &si, nullptr, &next), "vkCreateSwapchainKHR", error))
                return false;
            if (swapchain)
                vkDestroySwapchainKHR(device, swapchain, nullptr);
            swapchain = next;
            swapFormat = fmt.format;
            swapExtent = {w, h}; // the size we were asked for (compared on the next call)
            vkGetSwapchainImagesKHR(device, swapchain, &n, nullptr);
            swapImages.resize(n);
            vkGetSwapchainImagesKHR(device, swapchain, &n, swapImages.data());
            realExtent = extent;
            return true;
        }
        VkExtent2D realExtent{};

        bool createPipelines(std::string &error)
        {
            // render pass: one RGBA8 colour target (read back afterwards) + D32 depth
            VkAttachmentDescription at[2]{};
            at[0].format = VK_FORMAT_R8G8B8A8_UNORM;
            at[0].samples = VK_SAMPLE_COUNT_1_BIT;
            at[0].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
            at[0].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
            at[0].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
            at[0].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
            at[0].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            at[0].finalLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
            at[1] = at[0];
            at[1].format = VK_FORMAT_D32_SFLOAT;
            at[1].storeOp = VK_ATTACHMENT_STORE_OP_STORE; // read back on request (renderOffscreen depthOut)
            at[1].finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
            VkAttachmentReference colourRef{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
            VkAttachmentReference depthRef{1, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
            VkSubpassDescription sub{};
            sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
            sub.colorAttachmentCount = 1;
            sub.pColorAttachments = &colourRef;
            sub.pDepthStencilAttachment = &depthRef;
            // the frame is read by transfers afterwards (readback copy, blit to the window)
            VkSubpassDependency dep{};
            dep.srcSubpass = 0;
            dep.dstSubpass = VK_SUBPASS_EXTERNAL;
            dep.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
            dep.dstStageMask = VK_PIPELINE_STAGE_TRANSFER_BIT;
            dep.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
            dep.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
            // and, for the pass resumed after a mid-frame copy (Draw::copyFrame), the copy's read of the colour and the
            // depth writes before it; both passes carry both dependencies (render pass compatibility wants them equal)
            VkSubpassDependency deps[2] = {dep, {}};
            deps[1].srcSubpass = VK_SUBPASS_EXTERNAL;
            deps[1].dstSubpass = 0;
            deps[1].srcStageMask = VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
            deps[1].dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT |
                                   VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
            deps[1].srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
            deps[1].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
                                    VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
            VkRenderPassCreateInfo rp{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
            rp.attachmentCount = 2;
            rp.pAttachments = at;
            rp.subpassCount = 1;
            rp.pSubpasses = &sub;
            rp.dependencyCount = 2;
            rp.pDependencies = deps;
            if (!check(vkCreateRenderPass(device, &rp, nullptr, &renderPass), "vkCreateRenderPass", error))
                return false;
            // resumed: colour and depth loaded where the first pass left them
            at[0].loadOp = at[1].loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
            at[0].initialLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
            at[1].initialLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
            if (!check(vkCreateRenderPass(device, &rp, nullptr, &renderPassLoad), "vkCreateRenderPass (load)", error))
                return false;

            VkDescriptorSetLayoutBinding b[2]{};
            for (uint32_t i = 0; i < 2; ++i)
            {
                b[i].binding = i;
                b[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
                b[i].descriptorCount = 1;
                b[i].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
            }
            VkDescriptorSetLayoutCreateInfo dl{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
            dl.bindingCount = 2;
            dl.pBindings = b;
            if (!check(vkCreateDescriptorSetLayout(device, &dl, nullptr, &setLayout), "vkCreateDescriptorSetLayout", error))
                return false;
            VkPushConstantRange pr{VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(Push)};
            VkPipelineLayoutCreateInfo pl{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
            pl.setLayoutCount = 1;
            pl.pSetLayouts = &setLayout;
            pl.pushConstantRangeCount = 1;
            pl.pPushConstantRanges = &pr;
            if (!check(vkCreatePipelineLayout(device, &pl, nullptr, &pipelineLayout), "vkCreatePipelineLayout", error))
                return false;

            vs = shader(kMeshVert, sizeof(kMeshVert), error);
            fs = shader(kMeshFrag, sizeof(kMeshFrag), error);
            return vs && fs;
        }

        // Pipelines per blend state, made on first use: key = blend kind (bits 0-3), destination
        // alpha (4), back-face culling (5), depth write (6), no depth test (7: GS ZTST ALWAYS), colour
        // masked (8) and alpha written (9) (GS FBMSK).
        std::unordered_map<uint32_t, VkPipeline> pipelines;
        VkShaderModule vs = VK_NULL_HANDLE, fs = VK_NULL_HANDLE;

        static uint32_t pipelineKey(const Material *mat)
        {
            if (!mat)
                return 1u << 6;
            const AlphaMode mode = mat->alphaMode == AlphaMode::Mask ? AlphaMode::Opaque : mat->alphaMode;
            const bool depthWrite = isBlended(mode) ? mat->depthWrite && !mat->legacyBlend : mat->depthWrite;
            return uint32_t(mode) | (mat->destAlpha ? 1u << 4 : 0u) | (!mat->doubleSided ? 1u << 5 : 0u) | (depthWrite ? 1u << 6 : 0u) |
                   (!mat->depthTest ? 1u << 7 : 0u) | (!mat->writeRgb ? 1u << 8 : 0u) | (mat->writeAlpha ? 1u << 9 : 0u);
        }

        VkPipeline pipeline(uint32_t key, std::string &error)
        {
            if (const auto it = pipelines.find(key); it != pipelines.end())
                return it->second;
            VkPipelineShaderStageCreateInfo stages[2]{};
            stages[0].sType = stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
            stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
            stages[0].module = vs;
            stages[0].pName = "main";
            stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
            stages[1].module = fs;
            stages[1].pName = "main";

            VkVertexInputBindingDescription vb{0, sizeof(Vertex), VK_VERTEX_INPUT_RATE_VERTEX};
            VkVertexInputAttributeDescription va[4] = {
                {0, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(Vertex, pos)},
                {1, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(Vertex, uv)},
                {2, 0, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(Vertex, colour)},
                {3, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(Vertex, uv2)},
            };
            VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
            vi.vertexBindingDescriptionCount = 1;
            vi.pVertexBindingDescriptions = &vb;
            vi.vertexAttributeDescriptionCount = 4;
            vi.pVertexAttributeDescriptions = va;
            VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
            ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
            VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
            vp.viewportCount = vp.scissorCount = 1;
            VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
            rs.polygonMode = VK_POLYGON_MODE_FILL;
            rs.lineWidth = 1.0f;
            if (key & (1u << 5))
            {
                rs.cullMode = VK_CULL_MODE_BACK_BIT;
                rs.frontFace = options.cullClockwise ? VK_FRONT_FACE_COUNTER_CLOCKWISE : VK_FRONT_FACE_CLOCKWISE;
            }
            else
            {
                rs.cullMode = options.cull ? VK_CULL_MODE_BACK_BIT : VK_CULL_MODE_NONE; // converter: all double-sided
                rs.frontFace = options.cull == 2 ? VK_FRONT_FACE_CLOCKWISE : VK_FRONT_FACE_COUNTER_CLOCKWISE;
            }
            VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
            ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
            VkPipelineDepthStencilStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
            ds.depthTestEnable = VK_TRUE;
            ds.depthWriteEnable = (key & (1u << 6)) ? VK_TRUE : VK_FALSE;
            // depth = near / w: nearer is larger
            // GS ZTST GEQUAL (TEST 0x50000), so the game's same-depth redraws (the glow mask pass) pass
            ds.depthCompareOp = (key & (1u << 7)) ? VK_COMPARE_OP_ALWAYS : VK_COMPARE_OP_GREATER_OR_EQUAL;
            VkPipelineColorBlendAttachmentState ba{};
            ba.colorWriteMask = ((key & (1u << 8)) ? 0u : (VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT)) |
                                ((key & (1u << 9)) ? VK_COLOR_COMPONENT_A_BIT : 0u);
            // GS (A - B) * C + D as src * S + dst * D; C = As (shader alpha) or Ad (destination alpha)
            const AlphaMode mode = AlphaMode(key & 0xF);
            const bool dest = (key & (1u << 4)) != 0;
            const VkBlendFactor c = dest ? VK_BLEND_FACTOR_DST_ALPHA : VK_BLEND_FACTOR_SRC_ALPHA;
            const VkBlendFactor c1 = dest ? VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA : VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
            float constants[4] = {0, 0, 0, 0};
            if (isBlended(mode))
            {
                ba.blendEnable = VK_TRUE;
                ba.colorBlendOp = VK_BLEND_OP_ADD;
                // the GS does not blend alpha: As is written where FBMSK allows (write mask above)
                ba.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
                ba.dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
                ba.alphaBlendOp = VK_BLEND_OP_ADD;
                switch (mode)
                {
                case AlphaMode::Blend: ba.srcColorBlendFactor = c; ba.dstColorBlendFactor = c1; break;
                case AlphaMode::InvBlend: ba.srcColorBlendFactor = c1; ba.dstColorBlendFactor = c; break;
                case AlphaMode::Add: ba.srcColorBlendFactor = VK_BLEND_FACTOR_ONE; ba.dstColorBlendFactor = VK_BLEND_FACTOR_ONE; break;
                case AlphaMode::AddAlpha: // As from the source: the shader writes Cs * As (bit 512), unclamped above 0x80
                    ba.srcColorBlendFactor = dest ? c : VK_BLEND_FACTOR_ONE;
                    ba.dstColorBlendFactor = VK_BLEND_FACTOR_ONE;
                    break;
                case AlphaMode::AddDest: ba.srcColorBlendFactor = VK_BLEND_FACTOR_ONE; ba.dstColorBlendFactor = c; break;
                case AlphaMode::Multiply: ba.srcColorBlendFactor = VK_BLEND_FACTOR_ZERO; ba.dstColorBlendFactor = c; break;
                case AlphaMode::InvMultiply: ba.srcColorBlendFactor = VK_BLEND_FACTOR_ZERO; ba.dstColorBlendFactor = c1; break;
                case AlphaMode::Subtract:
                    // Cd - Cs * FIX: FIX 0x30 is the only value the game uses (SetBlending kind 5)
                    ba.colorBlendOp = VK_BLEND_OP_REVERSE_SUBTRACT;
                    ba.srcColorBlendFactor = VK_BLEND_FACTOR_CONSTANT_COLOR;
                    ba.dstColorBlendFactor = VK_BLEND_FACTOR_ONE;
                    constants[0] = constants[1] = constants[2] = constants[3] = 0x30 / 128.0f;
                    break;
                default: break;
                }
            }
            VkPipelineColorBlendStateCreateInfo cbs{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
            cbs.attachmentCount = 1;
            cbs.pAttachments = &ba;
            std::memcpy(cbs.blendConstants, constants, sizeof constants);
            VkDynamicState dyn[2] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
            VkPipelineDynamicStateCreateInfo dy{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
            dy.dynamicStateCount = 2;
            dy.pDynamicStates = dyn;
            VkGraphicsPipelineCreateInfo gp{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
            gp.stageCount = 2;
            gp.pStages = stages;
            gp.pVertexInputState = &vi;
            gp.pInputAssemblyState = &ia;
            gp.pViewportState = &vp;
            gp.pRasterizationState = &rs;
            gp.pMultisampleState = &ms;
            gp.pDepthStencilState = &ds;
            gp.pColorBlendState = &cbs;
            gp.pDynamicState = &dy;
            gp.layout = pipelineLayout;
            gp.renderPass = renderPass;
            VkPipeline p = VK_NULL_HANDLE;
            if (!check(vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &gp, nullptr, &p), "pipeline", error))
                return VK_NULL_HANDLE;
            pipelines[key] = p;
            return p;
        }

        // ---- overlay: an image presented instead of the scene ----
        Texture overlay;
        uint32_t overlayW = 0, overlayH = 0;
        bool overlayOn = false;

        // ---- GPU post effects (present path) ----
        PostEffects post;
        VkDescriptorSetLayout postSetLayout = VK_NULL_HANDLE;
        VkPipelineLayout postLayout = VK_NULL_HANDLE;
        VkPipeline postSprite = VK_NULL_HANDLE, postDiffusion = VK_NULL_HANDLE, postConvert = VK_NULL_HANDLE;
        VkDescriptorPool postPool = VK_NULL_HANDLE;
        Buffer postFrame, postWork, postSnap, postFull, postAlpha, postDummy, postPrev; // postPrev: the last frame (FixBlend)
        bool postPrevValid = false;
        std::vector<Buffer> postPlanes; // ColorDiffusion levels: per level the plane and its convolved copy
        uint32_t postW = 0, postH = 0;

        bool createPostPipelines(std::string &error)
        {
            VkDescriptorSetLayoutBinding b[3]{};
            for (uint32_t i = 0; i < 3; ++i)
            {
                b[i].binding = i;
                b[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
                b[i].descriptorCount = 1;
                b[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
            }
            VkDescriptorSetLayoutCreateInfo dl{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
            dl.bindingCount = 3;
            dl.pBindings = b;
            if (!check(vkCreateDescriptorSetLayout(device, &dl, nullptr, &postSetLayout), "post set layout", error))
                return false;
            VkPushConstantRange pr{VK_SHADER_STAGE_COMPUTE_BIT, 0, 64};
            VkPipelineLayoutCreateInfo pl{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
            pl.setLayoutCount = 1;
            pl.pSetLayouts = &postSetLayout;
            pl.pushConstantRangeCount = 1;
            pl.pPushConstantRanges = &pr;
            if (!check(vkCreatePipelineLayout(device, &pl, nullptr, &postLayout), "post pipeline layout", error))
                return false;
            auto make = [&](const uint32_t *code, size_t bytes, VkPipeline &out) {
                VkShaderModule mod = shader(code, bytes, error);
                if (!mod)
                    return false;
                VkComputePipelineCreateInfo ci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
                ci.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
                ci.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
                ci.stage.module = mod;
                ci.stage.pName = "main";
                ci.layout = postLayout;
                const bool ok = check(vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &ci, nullptr, &out), "post pipeline", error);
                vkDestroyShaderModule(device, mod, nullptr);
                return ok;
            };
            VkDescriptorPoolSize ps{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 3 * 128};
            VkDescriptorPoolCreateInfo dp{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
            dp.maxSets = 128;
            dp.poolSizeCount = 1;
            dp.pPoolSizes = &ps;
            return make(kGsSpriteComp, sizeof(kGsSpriteComp), postSprite) && make(kColorDiffusionComp, sizeof(kColorDiffusionComp), postDiffusion) &&
                   make(kRgbaFloatComp, sizeof(kRgbaFloatComp), postConvert) &&
                   check(vkCreateDescriptorPool(device, &dp, nullptr, &postPool), "post descriptor pool", error);
        }

        void destroyPostBuffers()
        {
            for (Buffer *b : {&postFrame, &postWork, &postSnap, &postFull, &postAlpha, &postDummy, &postPrev})
                destroyBuffer(*b);
            for (Buffer &b : postPlanes)
                destroyBuffer(b);
            postPlanes.clear();
            postW = postH = 0;
        }

        bool postOn() const { return post.glow || post.haze || post.motionSmooth || post.fixBlend > 0 || post.zoomSmooth > 0.0f; }
        bool ensurePostBuffers(uint32_t w, uint32_t h, std::string &error)
        {
            if (postW == w && postH == h)
                return true;
            vkDeviceWaitIdle(device);
            destroyPostBuffers();
            const uint32_t scale = std::max(1u, w / 640);
            const VkDeviceSize work = VkDeviceSize(448 * scale) * (288 * scale) * 4, frame = VkDeviceSize(w) * h * 4;
            const VkBufferUsageFlags u = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
            const VkMemoryPropertyFlags d = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
            bool ok = createBuffer(frame, u, d, postFrame, error) && createBuffer(work, u, d, postWork, error) &&
                      createBuffer(std::max(frame, work), u, d, postSnap, error) && createBuffer(frame * 4, u, d, postFull, error) &&
                      createBuffer(frame * 4, u, d, postAlpha, error) && createBuffer(64, u, d, postDummy, error) &&
                      createBuffer(frame, u, d, postPrev, error);
            postPrevValid = false;
            // ColorDiffusion planes: level 0 at half size (+ its resampled mask), then halvings, each with
            // a convolved copy: at most 2 per level, 10 levels
            uint32_t lw = w / 2, lh = h / 2;
            for (int l = 0; ok && l < 10 && lw >= 1 && lh >= 1; ++l, lw /= 2, lh /= 2)
                for (int k = 0; k < 2; ++k)
                {
                    postPlanes.emplace_back();
                    ok = createBuffer(VkDeviceSize(lw) * lh * 16, u, d, postPlanes.back(), error);
                }
            if (!ok)
                return false;
            postW = w;
            postH = h;
            // the work buffer starts black (the haze reads a texel past some regions it wrote)
            return oneShot([&](VkCommandBuffer cb) { vkCmdFillBuffer(cb, postWork.buffer, 0, VK_WHOLE_SIZE, 0); }, error);
        }

        static void memoryBarrier(VkCommandBuffer cb, VkPipelineStageFlags src, VkAccessFlags srcAccess, VkPipelineStageFlags dst,
                                  VkAccessFlags dstAccess)
        {
            VkMemoryBarrier mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
            mb.srcAccessMask = srcAccess;
            mb.dstAccessMask = dstAccess;
            vkCmdPipelineBarrier(cb, src, dst, 0, 1, &mb, 0, nullptr, 0, nullptr);
        }

        void dispatchPost(VkCommandBuffer cb, VkPipeline pipe, VkBuffer b0, VkBuffer b1, VkBuffer b2, const void *push, size_t bytes,
                          uint32_t w, uint32_t h)
        {
            VkDescriptorSetAllocateInfo da{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
            da.descriptorPool = postPool;
            da.descriptorSetCount = 1;
            da.pSetLayouts = &postSetLayout;
            VkDescriptorSet set = VK_NULL_HANDLE;
            if (vkAllocateDescriptorSets(device, &da, &set) != VK_SUCCESS)
                return;
            VkDescriptorBufferInfo info[3] = {{b0, 0, VK_WHOLE_SIZE}, {b1, 0, VK_WHOLE_SIZE}, {b2, 0, VK_WHOLE_SIZE}};
            VkWriteDescriptorSet wr[3]{};
            for (uint32_t i = 0; i < 3; ++i)
            {
                wr[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
                wr[i].dstSet = set;
                wr[i].dstBinding = i;
                wr[i].descriptorCount = 1;
                wr[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
                wr[i].pBufferInfo = &info[i];
            }
            vkUpdateDescriptorSets(device, 3, wr, 0, nullptr);
            vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pipe);
            vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, postLayout, 0, 1, &set, 0, nullptr);
            uint8_t pc[64] = {};
            std::memcpy(pc, push, std::min<size_t>(bytes, 64));
            vkCmdPushConstants(cb, postLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, 64, pc);
            vkCmdDispatch(cb, (w + 7) / 8, (h + 7) / 8, 1);
            memoryBarrier(cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT,
                          VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT,
                          VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_READ_BIT);
        }

        // the post chain on `image` (w x h, in TRANSFER_SRC layout before and after): copy to a buffer,
        // ColorDiffusion and/or SmartBlur as compute dispatches, copy back. The descriptor pool is reset
        // here, so only one frame may be in flight (present waits for the previous one first).
        void recordPost(VkCommandBuffer cb, VkImage image, uint32_t w, uint32_t h)
        {
            vkResetDescriptorPool(device, postPool, 0);
            VkBufferImageCopy region{};
            region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            region.imageExtent = {w, h, 1};
            vkCmdCopyImageToBuffer(cb, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, postFrame.buffer, 1, &region);
            memoryBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                          VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);
            struct SpritePush
            {
                float xy[4], uv[4];
                int32_t size[4];
                uint32_t colour, flags;
            };
            const uint32_t scale = std::max(1u, w / 640);
            // full-frame blits (pop_postfx::Blit): surfaces frame, temp (postSnap), previous frame
            auto runBlits = [&](const std::vector<pop_postfx::Blit> &blits) {
                for (const pop_postfx::Blit &b : blits)
                {
                    const VkBuffer surf[3] = {postFrame.buffer, postSnap.buffer, postPrev.buffer};
                    SpritePush p{{b.x0, b.y0, b.x1, b.y1}, {b.u0, b.v0, b.u1, b.v1}, {int32_t(w), int32_t(h), int32_t(w), int32_t(h)}, b.colour,
                                 (b.src >= 0 ? 1u : 0u) | (b.blend == pop_postfx::Blend::Lerp44 ? 2u : 0u) | (b.blend == pop_postfx::Blend::Add ? 4u : 0u)};
                    dispatchPost(cb, postSprite, b.src >= 0 ? surf[b.src] : postDummy.buffer, surf[b.dst], postDummy.buffer, &p, sizeof p, w, h);
                }
            };
            // ZoomSmooth 0 runs before the glow in the game's dispatcher
            if (post.zoomSmooth > 0.0f)
                runBlits(pop_postfx::zoomSmoothBlits(post.zoomSmooth, int(w), int(h), int(scale)));
            if (post.glow && post.levels >= 2 && w >= 4 && h >= 4)
            {
                struct ConvPush
                {
                    int32_t op, w, h;
                } cvt{0, int32_t(w), int32_t(h)};
                dispatchPost(cb, postConvert, postFrame.buffer, postFull.buffer, postAlpha.buffer, &cvt, sizeof cvt, w, h);
                struct DiffPush
                {
                    int32_t op, sw, sh, dw, dh, strength;
                    float k;
                };
                auto run = [&](int op, VkBuffer src, uint32_t sw, uint32_t sh, VkBuffer dst, uint32_t dw, uint32_t dh, VkBuffer aux, float k) {
                    DiffPush p{op, int32_t(sw), int32_t(sh), int32_t(dw), int32_t(dh), post.strength, k};
                    dispatchPost(cb, postDiffusion, src, dst, aux, &p, sizeof p, dw, dh);
                };
                // level 0: half size, masked by the resampled frame alpha (postPlanes[0], mask in [1])
                uint32_t lw = w / 2, lh = h / 2;
                VkBuffer level = postPlanes[0].buffer;
                run(0, postFull.buffer, w, h, level, lw, lh, postDummy.buffer, 0);
                if (post.mask)
                {
                    run(0, postAlpha.buffer, w, h, postPlanes[1].buffer, lw, lh, postDummy.buffer, 0);
                    run(1, postDummy.buffer, lw, lh, level, lw, lh, postPlanes[1].buffer, 0);
                }
                struct L
                {
                    VkBuffer b;
                    uint32_t w, h;
                };
                std::vector<L> pyramid;
                for (int l = 1; l < post.levels && lw >= 2 && lh >= 2 && size_t(2 * l + 1) < postPlanes.size(); ++l)
                {
                    const uint32_t nw = lw / 2, nh = lh / 2;
                    VkBuffer next = postPlanes[size_t(2 * l)].buffer;
                    run(0, level, lw, lh, next, nw, nh, postDummy.buffer, 0);
                    level = next;
                    lw = nw;
                    lh = nh;
                    if (post.convolution)
                    {
                        VkBuffer conv = postPlanes[size_t(2 * l + 1)].buffer;
                        run(2, level, lw, lh, conv, lw, lh, postDummy.buffer, 0);
                        level = conv;
                    }
                    pyramid.push_back({level, lw, lh});
                }
                const float k = float(post.fix & 0xFF) / 128.0f;
                for (int l = int(pyramid.size()) - 1; l >= 0; --l)
                    run(3, pyramid[size_t(l)].b, pyramid[size_t(l)].w, pyramid[size_t(l)].h, postFull.buffer, w, h, postDummy.buffer, k);
                cvt.op = 1;
                dispatchPost(cb, postConvert, postFrame.buffer, postFull.buffer, postAlpha.buffer, &cvt, sizeof cvt, w, h);
            }
            // MotionSmooth, then FixBlend (the previous frame over this one)
            std::vector<pop_postfx::Blit> blits;
            if (post.motionSmooth)
                blits = pop_postfx::motionSmoothBlits(post.motionDx, post.motionDy, int(w), int(h), int(scale));
            if (post.fixBlend > 0 && postPrevValid)
                blits.push_back(pop_postfx::fixBlendBlit(post.fixBlend, int(w), int(h)));
            runBlits(blits);
            if (post.haze)
            {
                const uint32_t ww = 448 * scale, wh = 288 * scale;
                const float kk = float(scale) / 16.0f;
                const int a = int(float(post.hazeColour >> 24) * (1.0f - post.hazeFade));
                const pop_postfx::SmartBlurBlit *bl = pop_postfx::smartBlurBlits();
                for (int i = 0; i < 11; ++i)
                {
                    const auto &b = bl[i];
                    const VkBuffer dst = b.toFrame ? postFrame.buffer : postWork.buffer;
                    const uint32_t dw = b.toFrame ? w : ww, dh = b.toFrame ? h : wh;
                    VkBuffer src = b.fromFrame ? postFrame.buffer : postWork.buffer;
                    const uint32_t sw = b.fromFrame ? w : ww, sh = b.fromFrame ? h : wh;
                    if (b.textured && src == dst)
                    {
                        // a blit that reads its own target reads a copy of it (as the CPU reference)
                        VkBufferCopy c{0, 0, VkDeviceSize(dw) * dh * 4};
                        vkCmdCopyBuffer(cb, dst, postSnap.buffer, 1, &c);
                        memoryBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                                      VK_ACCESS_SHADER_READ_BIT);
                        src = postSnap.buffer;
                    }
                    SpritePush p{};
                    p.xy[0] = (b.x0 - 0x6C00) * kk;
                    p.xy[1] = (b.y0 - 0x7200) * kk;
                    p.xy[2] = (b.x1 - 0x6C00) * kk;
                    p.xy[3] = (b.y1 - 0x7200) * kk;
                    p.uv[0] = b.u0 * kk;
                    p.uv[1] = b.v0 * kk;
                    p.uv[2] = b.u1 * kk;
                    p.uv[3] = b.v1 * kk;
                    p.size[0] = int32_t(sw);
                    p.size[1] = int32_t(sh);
                    p.size[2] = int32_t(dw);
                    p.size[3] = int32_t(dh);
                    p.colour = b.colour == 0xFFFFFFFFu ? ((post.hazeColour & 0xFFFFFFu) | (uint32_t(a & 0xFF) << 24)) : b.colour;
                    p.flags = (b.textured ? 1u : 0u) | (b.blend ? 2u : 0u);
                    dispatchPost(cb, postSprite, b.textured ? src : postDummy.buffer, dst, postDummy.buffer, &p, sizeof p, dw, dh);
                }
            }
            // the finished frame for the next one's FixBlend (the game's other draw buffer; ours has no HUD in it)
            const VkBufferCopy keep{0, 0, VkDeviceSize(w) * h * 4};
            vkCmdCopyBuffer(cb, postFrame.buffer, postPrev.buffer, 1, &keep);
            postPrevValid = true;
            barrier(cb, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_ACCESS_TRANSFER_READ_BIT,
                    VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
            vkCmdCopyBufferToImage(cb, postFrame.buffer, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
            barrier(cb, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT,
                    VK_ACCESS_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
        }
    };

    Renderer::Renderer() : m(new Impl) {}

    Renderer::~Renderer()
    {
        if (m->device)
        {
            releaseOffscreen();
            vkDeviceWaitIdle(m->device);
            m->dropKept();
            if (m->targetFb)
                vkDestroyFramebuffer(m->device, m->targetFb, nullptr);
            m->destroyImage(m->targetColour);
            m->destroyImage(m->targetDepth);
            if (m->swapchain)
                vkDestroySwapchainKHR(m->device, m->swapchain, nullptr);
            if (m->frameFence)
                vkDestroyFence(m->device, m->frameFence, nullptr);
            if (m->acquired)
                vkDestroySemaphore(m->device, m->acquired, nullptr);
            if (m->rendered)
                vkDestroySemaphore(m->device, m->rendered, nullptr);
            m->destroyBuffer(m->vertexBuffer);
            m->destroyBuffer(m->indexBuffer);
            m->destroyPostBuffers();
            m->destroyImage(m->overlay);
            for (VkPipeline p : {m->postSprite, m->postDiffusion, m->postConvert})
                if (p)
                    vkDestroyPipeline(m->device, p, nullptr);
            if (m->postPool)
                vkDestroyDescriptorPool(m->device, m->postPool, nullptr);
            if (m->postLayout)
                vkDestroyPipelineLayout(m->device, m->postLayout, nullptr);
            if (m->postSetLayout)
                vkDestroyDescriptorSetLayout(m->device, m->postSetLayout, nullptr);
            for (Texture &t : m->textures)
                m->destroyImage(t);
            if (m->descriptorPool)
                vkDestroyDescriptorPool(m->device, m->descriptorPool, nullptr);
            for (const auto &kv : m->pipelines)
                vkDestroyPipeline(m->device, kv.second, nullptr);
            if (m->vs)
                vkDestroyShaderModule(m->device, m->vs, nullptr);
            if (m->fs)
                vkDestroyShaderModule(m->device, m->fs, nullptr);
            if (m->pipelineLayout)
                vkDestroyPipelineLayout(m->device, m->pipelineLayout, nullptr);
            if (m->setLayout)
                vkDestroyDescriptorSetLayout(m->device, m->setLayout, nullptr);
            if (m->renderPass)
                vkDestroyRenderPass(m->device, m->renderPass, nullptr);
            if (m->renderPassLoad)
                vkDestroyRenderPass(m->device, m->renderPassLoad, nullptr);
            m->destroyImage(m->frameCopy);
            m->destroyImage(m->frameSave);
            if (m->sampler)
                vkDestroySampler(m->device, m->sampler, nullptr);
            if (m->clampSampler)
                vkDestroySampler(m->device, m->clampSampler, nullptr);
            if (m->commandPool)
                vkDestroyCommandPool(m->device, m->commandPool, nullptr);
            vkDestroyDevice(m->device, nullptr);
        }
        if (m->surface)
            vkDestroySurfaceKHR(m->instance, m->surface, nullptr);
        if (m->instance)
            vkDestroyInstance(m->instance, nullptr);
        delete m;
    }

    std::string Renderer::deviceName() const { return m->props.deviceName; }

    void Renderer::setFog(const Fog &fog) { m->fog = fog; }
    void Renderer::setPostEffects(const PostEffects &post) { m->post = post; }

    bool Renderer::setOverlay(const Image *image, std::string &error)
    {
        if (!image || !image->width || !image->height)
        {
            m->overlayOn = false;
            return true;
        }
        if (m->frameFence)
            vkWaitForFences(m->device, 1, &m->frameFence, VK_TRUE, UINT64_MAX);
        if (m->overlayW != image->width || m->overlayH != image->height)
        {
            vkDeviceWaitIdle(m->device);
            m->destroyImage(m->overlay);
            if (!m->createImage(image->width, image->height, VK_FORMAT_R8G8B8A8_UNORM,
                                VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, VK_IMAGE_ASPECT_COLOR_BIT, m->overlay, error))
                return false;
            m->overlayW = image->width;
            m->overlayH = image->height;
        }
        Buffer staging;
        if (!m->createBuffer(image->rgba.size(), VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                             VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, staging, error))
            return false;
        void *p;
        vkMapMemory(m->device, staging.memory, 0, image->rgba.size(), 0, &p);
        std::memcpy(p, image->rgba.data(), image->rgba.size());
        vkUnmapMemory(m->device, staging.memory);
        const bool ok = m->oneShot([&](VkCommandBuffer cb) {
            Impl::barrier(cb, m->overlay.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT,
                          VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
            VkBufferImageCopy region{};
            region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            region.imageExtent = {image->width, image->height, 1};
            vkCmdCopyBufferToImage(cb, staging.buffer, m->overlay.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
            Impl::barrier(cb, m->overlay.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                          VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
        }, error);
        m->destroyBuffer(staging);
        m->overlayOn = ok;
        return ok;
    }

    bool Renderer::readPresented(std::vector<uint8_t> &rgba, uint32_t &width, uint32_t &height, std::string &error)
    {
        if (!m->targetFb)
        {
            error = "nothing presented yet";
            return false;
        }
        if (m->frameFence)
            vkWaitForFences(m->device, 1, &m->frameFence, VK_TRUE, UINT64_MAX);
        width = m->targetW;
        height = m->targetH;
        Buffer readback;
        if (!m->createBuffer(VkDeviceSize(width) * height * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                             VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, readback, error))
            return false;
        const bool ok = m->oneShot([&](VkCommandBuffer cb) {
            VkBufferImageCopy region{};
            region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            region.imageExtent = {width, height, 1};
            vkCmdCopyImageToBuffer(cb, m->targetColour.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, readback.buffer, 1, &region);
        }, error);
        if (ok)
        {
            void *p;
            vkMapMemory(m->device, readback.memory, 0, readback.size, 0, &p);
            rgba.assign(static_cast<uint8_t *>(p), static_cast<uint8_t *>(p) + size_t(width) * height * 4);
            vkUnmapMemory(m->device, readback.memory);
        }
        m->destroyBuffer(readback);
        return ok;
    }

    bool Renderer::init(const Options &options, std::string &error)
    {
        m->options = options;
        VkResult loaded = volkInitialize();
#if defined(__APPLE__)
        // Homebrew's vulkan-loader is not on the default dyld search path
        if (loaded != VK_SUCCESS)
            for (const char *path : {"/opt/homebrew/lib/libvulkan.1.dylib", "/usr/local/lib/libvulkan.1.dylib"})
                if (void *lib = dlopen(path, RTLD_NOW | RTLD_LOCAL))
                {
                    volkInitializeCustom(reinterpret_cast<PFN_vkGetInstanceProcAddr>(dlsym(lib, "vkGetInstanceProcAddr")));
                    loaded = VK_SUCCESS;
                    break;
                }
#endif
        if (!check(loaded, "volkInitialize (no Vulkan loader?)", error))
            return false;
        // portability enumeration lets the loader list MoltenVK (macOS); harmless elsewhere
        uint32_t extCount = 0;
        vkEnumerateInstanceExtensionProperties(nullptr, &extCount, nullptr);
        std::vector<VkExtensionProperties> exts(extCount);
        vkEnumerateInstanceExtensionProperties(nullptr, &extCount, exts.data());
        std::vector<const char *> enable;
        bool portability = false;
        for (const auto &e : exts)
            if (std::strcmp(e.extensionName, VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME) == 0)
                portability = true;
        if (portability)
            enable.push_back(VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME);
        for (const std::string &e : options.instanceExtensions)
            enable.push_back(e.c_str());
        VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
        app.pApplicationName = "pop_native";
        app.apiVersion = VK_API_VERSION_1_1;
        VkInstanceCreateInfo ii{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
        ii.pApplicationInfo = &app;
        // POP_VK_VALIDATION=1: the Khronos validation layer (messages go to stdout without a messenger)
        const char *validation = "VK_LAYER_KHRONOS_validation";
        if (std::getenv("POP_VK_VALIDATION"))
        {
            uint32_t layerCount = 0;
            vkEnumerateInstanceLayerProperties(&layerCount, nullptr);
            std::vector<VkLayerProperties> layers(layerCount);
            vkEnumerateInstanceLayerProperties(&layerCount, layers.data());
            if (std::any_of(layers.begin(), layers.end(), [&](const VkLayerProperties &l) { return std::strcmp(l.layerName, validation) == 0; }))
            {
                ii.enabledLayerCount = 1;
                ii.ppEnabledLayerNames = &validation;
            }
            else
                std::fprintf(stderr, "POP_VK_VALIDATION: %s not installed\n", validation);
        }
        ii.enabledExtensionCount = static_cast<uint32_t>(enable.size());
        ii.ppEnabledExtensionNames = enable.data();
        if (portability)
            ii.flags |= VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR;
        if (!check(vkCreateInstance(&ii, nullptr, &m->instance), "vkCreateInstance", error))
            return false;
        volkLoadInstance(m->instance);
        if (options.createSurface)
        {
            m->surface = reinterpret_cast<VkSurfaceKHR>(options.createSurface(m->instance));
            if (!m->surface)
            {
                error = "the window system could not create a Vulkan surface";
                return false;
            }
        }

        uint32_t n = 0;
        vkEnumeratePhysicalDevices(m->instance, &n, nullptr);
        std::vector<VkPhysicalDevice> devs(n);
        vkEnumeratePhysicalDevices(m->instance, &n, devs.data());
        // prefer a discrete GPU with a graphics queue
        int best = -1;
        for (uint32_t i = 0; i < n; ++i)
        {
            VkPhysicalDeviceProperties p;
            vkGetPhysicalDeviceProperties(devs[i], &p);
            uint32_t qn = 0;
            vkGetPhysicalDeviceQueueFamilyProperties(devs[i], &qn, nullptr);
            std::vector<VkQueueFamilyProperties> qs(qn);
            vkGetPhysicalDeviceQueueFamilyProperties(devs[i], &qn, qs.data());
            for (uint32_t q = 0; q < qn; ++q)
            {
                VkBool32 canPresent = VK_TRUE;
                if (m->surface)
                    vkGetPhysicalDeviceSurfaceSupportKHR(devs[i], q, m->surface, &canPresent);
                if ((qs[q].queueFlags & VK_QUEUE_GRAPHICS_BIT) && canPresent)
                {
                    if (best < 0 || p.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU)
                    {
                        best = static_cast<int>(i);
                        m->queueFamily = q;
                    }
                    break;
                }
            }
        }
        if (best < 0)
        {
            error = "no Vulkan device with a graphics queue";
            return false;
        }
        m->physical = devs[best];
        vkGetPhysicalDeviceProperties(m->physical, &m->props);

        uint32_t dn = 0;
        vkEnumerateDeviceExtensionProperties(m->physical, nullptr, &dn, nullptr);
        std::vector<VkExtensionProperties> dexts(dn);
        vkEnumerateDeviceExtensionProperties(m->physical, nullptr, &dn, dexts.data());
        std::vector<const char *> denable;
        for (const auto &e : dexts)
            if (std::strcmp(e.extensionName, "VK_KHR_portability_subset") == 0)
                denable.push_back("VK_KHR_portability_subset");
        if (m->surface)
            denable.push_back(VK_KHR_SWAPCHAIN_EXTENSION_NAME);
        const float prio = 1.0f;
        VkDeviceQueueCreateInfo qi{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
        qi.queueFamilyIndex = m->queueFamily;
        qi.queueCount = 1;
        qi.pQueuePriorities = &prio;
        VkDeviceCreateInfo di{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
        di.queueCreateInfoCount = 1;
        di.pQueueCreateInfos = &qi;
        di.enabledExtensionCount = static_cast<uint32_t>(denable.size());
        di.ppEnabledExtensionNames = denable.data();
        if (!check(vkCreateDevice(m->physical, &di, nullptr, &m->device), "vkCreateDevice", error))
            return false;
        volkLoadDevice(m->device);
        vkGetDeviceQueue(m->device, m->queueFamily, 0, &m->queue);

        VkCommandPoolCreateInfo cp{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        cp.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        cp.queueFamilyIndex = m->queueFamily;
        if (!check(vkCreateCommandPool(m->device, &cp, nullptr, &m->commandPool), "vkCreateCommandPool", error))
            return false;
        VkSamplerCreateInfo si{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
        si.magFilter = si.minFilter = options.linearFilter ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
        si.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
        si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
        si.maxLod = 0.0f;
        if (!check(vkCreateSampler(m->device, &si, nullptr, &m->sampler), "vkCreateSampler", error))
            return false;
        si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        if (!check(vkCreateSampler(m->device, &si, nullptr, &m->clampSampler), "vkCreateSampler", error))
            return false;
        return m->createPipelines(error) && m->createPostPipelines(error);
    }

    int Renderer::addImage(const Image &image)
    {
        // index among all images ever added (textures[0] is white, so uploaded images are textures.size() - 1)
        m->images.push_back(image);
        return static_cast<int>((m->textures.empty() ? 0 : m->textures.size() - 1) + m->images.size()) - 1;
    }

    int Renderer::addMaterial(const Material &material)
    {
        m->materials.push_back(material);
        return static_cast<int>(m->materials.size()) - 1;
    }

    int Renderer::addMesh(Mesh mesh)
    {
        m->meshes.push_back(std::move(mesh));
        return static_cast<int>(m->meshes.size()) - 1;
    }

    int Renderer::keepMesh(uint64_t key, Mesh mesh)
    {
        const auto it = m->keptOf.find(key);
        if (it != m->keptOf.end())
            return it->second;
        const uint32_t v0 = m->keptVertexCount + uint32_t(m->keptPendingVertices.size());
        const uint32_t i0 = m->keptIndexCount + uint32_t(m->keptPendingIndices.size());
        m->kept.push_back({i0, uint32_t(mesh.indices.size()), int32_t(v0)});
        m->keptPendingVertices.insert(m->keptPendingVertices.end(), mesh.vertices.begin(), mesh.vertices.end());
        m->keptPendingIndices.insert(m->keptPendingIndices.end(), mesh.indices.begin(), mesh.indices.end());
        const int id = int(m->kept.size()) - 1;
        m->keptOf.emplace(key, id);
        return id;
    }

    int Renderer::keptMesh(uint64_t key) const
    {
        const auto it = m->keptOf.find(key);
        return it == m->keptOf.end() ? -1 : it->second;
    }

    uint32_t Renderer::keptEpoch() const { return m->keptEpoch; }
    void Renderer::dropKept()
    {
        if (!m->device)
            return;
        if (m->frameFence)
            vkWaitForFences(m->device, 1, &m->frameFence, VK_TRUE, UINT64_MAX); // the last frame still draws from them
        vkDeviceWaitIdle(m->device);
        m->dropKept();
    }

    size_t Renderer::keptBytes() const
    {
        return (m->keptVertexCount + m->keptPendingVertices.size()) * sizeof(Vertex) +
               (m->keptIndexCount + m->keptPendingIndices.size()) * sizeof(uint32_t);
    }

    size_t Renderer::textureCount() const
    {
        return m->textures.size();
    }

    size_t Renderer::textureBytes() const
    {
        size_t bytes = 0;
        for (const auto &t : m->textures)
            bytes += size_t(t.width) * t.height * 4; // RGBA8, one mip level
        return bytes;
    }

    bool Renderer::updateImage(int index, const Image &image)
    {
        const size_t uploaded = m->textures.empty() ? 0 : m->textures.size() - 1;
        if (index < 0)
            return false;
        if (size_t(index) >= uploaded)
        {
            // still waiting for upload(): replace it there
            const size_t pending = size_t(index) - uploaded;
            if (pending >= m->images.size())
                return false;
            m->images[pending] = image;
            return true;
        }
        const Texture &t = m->textures[size_t(index) + 1];
        if (t.width != image.width || t.height != image.height || image.rgba.size() != size_t(image.width) * image.height * 4)
            return false;
        m->imageUpdates.emplace_back(index, image);
        return true;
    }

    bool Renderer::upload(std::string &error)
    {
        // textures persist across reset(true): textures[0] is white (missing texture / no lightmap),
        // textures[1 + i] is image i; only the images added since the last upload are uploaded
        if (m->textures.empty())
        {
            m->textures.resize(1);
            Image white{1, 1, {255, 255, 255, 255}};
            if (!m->uploadTexture(white, m->textures[0], error))
                return false;
        }
        if (!m->images.empty())
        {
            const size_t first = m->textures.size();
            m->textures.resize(first + m->images.size());
            if (!m->uploadTextures(m->images, m->textures.data() + first, error))
                return false;
            m->images.clear();
            m->images.shrink_to_fit();
        }
        if (!m->imageUpdates.empty() && !m->rewriteTextures(error))
            return false;

        // descriptor sets: one per material (base, lightmap) plus one all-white
        const uint32_t sets = static_cast<uint32_t>(m->materials.size()) + 1;
        VkDescriptorPoolSize ps{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, sets * 2};
        VkDescriptorPoolCreateInfo dp{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        dp.maxSets = sets;
        dp.poolSizeCount = 1;
        dp.pPoolSizes = &ps;
        if (!check(vkCreateDescriptorPool(m->device, &dp, nullptr, &m->descriptorPool), "vkCreateDescriptorPool", error))
            return false;
        std::vector<VkDescriptorSetLayout> layouts(sets, m->setLayout);
        std::vector<VkDescriptorSet> all(sets);
        VkDescriptorSetAllocateInfo da{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        da.descriptorPool = m->descriptorPool;
        da.descriptorSetCount = sets;
        da.pSetLayouts = layouts.data();
        if (!check(vkAllocateDescriptorSets(m->device, &da, all.data()), "vkAllocateDescriptorSets", error))
            return false;
        auto texView = [&](int image) {
            return m->textures[image >= 0 && static_cast<size_t>(image) + 1 < m->textures.size() ? size_t(image) + 1 : 0].view;
        };
        for (uint32_t s = 0; s < sets; ++s)
        {
            const Material *mat = s < m->materials.size() ? &m->materials[s] : nullptr;
            VkDescriptorImageInfo info[2] = {
                {mat && mat->clampUv ? m->clampSampler : m->sampler, texView(mat ? mat->baseImage : -1), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
                {m->sampler, texView(mat ? mat->lightmapImage : -1), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
            };
            VkWriteDescriptorSet w[2]{};
            for (uint32_t k = 0; k < 2; ++k)
            {
                w[k].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
                w[k].dstSet = all[s];
                w[k].dstBinding = k;
                w[k].descriptorCount = 1;
                w[k].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
                w[k].pImageInfo = &info[k];
            }
            vkUpdateDescriptorSets(m->device, 2, w, 0, nullptr);
        }
        m->whiteSet = all.back();
        all.pop_back();
        m->materialSets = std::move(all);

        // all meshes in one vertex buffer and one index buffer
        std::vector<Vertex> verts;
        std::vector<uint32_t> idx;
        for (const Mesh &mesh : m->meshes)
        {
            m->ranges.push_back({static_cast<uint32_t>(idx.size()), static_cast<uint32_t>(mesh.indices.size()),
                                 static_cast<int32_t>(verts.size()), mesh.material});
            verts.insert(verts.end(), mesh.vertices.begin(), mesh.vertices.end());
            idx.insert(idx.end(), mesh.indices.begin(), mesh.indices.end());
        }
        m->meshes.clear();
        return m->uploadBuffer(verts.data(), verts.size() * sizeof(Vertex), VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, m->vertexBuffer, error) &&
               m->uploadBuffer(idx.data(), idx.size() * sizeof(uint32_t), VK_BUFFER_USAGE_INDEX_BUFFER_BIT, m->indexBuffer, error) &&
               m->uploadKept(autoDropKept ? keptBudgetBytes : ~size_t(0), error);
    }

    void Renderer::reset(bool keepImages)
    {
        if (!m->device)
            return;
        if (m->frameFence)
            vkWaitForFences(m->device, 1, &m->frameFence, VK_TRUE, UINT64_MAX); // the last frame still uses the scene
        vkDeviceWaitIdle(m->device);
        m->destroyBuffer(m->vertexBuffer);
        m->destroyBuffer(m->indexBuffer);
        if (!keepImages)
        {
            for (Texture &t : m->textures)
                m->destroyImage(t);
            m->textures.clear();
            m->images.clear();
            m->imageUpdates.clear();
            m->dropKept();
        }
        if (m->descriptorPool)
            vkDestroyDescriptorPool(m->device, m->descriptorPool, nullptr);
        m->descriptorPool = VK_NULL_HANDLE;
        m->materialSets.clear();
        m->whiteSet = VK_NULL_HANDLE;
        m->materials.clear();
        m->meshes.clear();
        m->ranges.clear();
    }

    void Renderer::releaseOffscreen()
    {
        auto &o = m->off;
        if (!m->device)
            return;
        vkDeviceWaitIdle(m->device);
        if (o.fb)
            vkDestroyFramebuffer(m->device, o.fb, nullptr);
        if (o.colourMap)
            vkUnmapMemory(m->device, o.readback.memory);
        if (o.depthMap)
            vkUnmapMemory(m->device, o.depthReadback.memory);
        m->destroyImage(o.colour);
        m->destroyImage(o.depth);
        m->destroyBuffer(o.readback);
        m->destroyBuffer(o.depthReadback);
        o = {};
    }

    bool Renderer::renderOffscreen(uint32_t width, uint32_t height, const float clip[16], const std::vector<Draw> &draws,
                                   std::vector<uint8_t> &rgbaOut, std::string &error, std::vector<float> *depthOut,
                                   std::vector<uint32_t> *gsZOut, const std::vector<uint8_t> *under)
    {
        auto &o = m->off;
        bool ok = true;
        if (o.w != width || o.h != height)
        {
            releaseOffscreen();
            const VkDeviceSize bytes = VkDeviceSize(width) * height * 4;
            const VkMemoryPropertyFlags cachedFlags = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_CACHED_BIT;
            const VkMemoryPropertyFlags plainFlags = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
            std::string ignored;
            o.cached = m->createBuffer(bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT, cachedFlags, o.readback, ignored) &&
                       m->createBuffer(bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT, cachedFlags, o.depthReadback, ignored);
            if (!o.cached)
            {
                m->destroyBuffer(o.readback);
                m->destroyBuffer(o.depthReadback);
                ok = m->createBuffer(bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT, plainFlags, o.readback, error) &&
                     m->createBuffer(bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT, plainFlags, o.depthReadback, error);
            }
            ok = ok &&
                 m->createImage(width, height, VK_FORMAT_R8G8B8A8_UNORM,
                                VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                                VK_IMAGE_ASPECT_COLOR_BIT, o.colour, error) &&
                 m->createImage(width, height, VK_FORMAT_D32_SFLOAT, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                                VK_IMAGE_ASPECT_DEPTH_BIT, o.depth, error);
            if (ok)
            {
                VkImageView views[2] = {o.colour.view, o.depth.view};
                VkFramebufferCreateInfo fi{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
                fi.renderPass = m->renderPass;
                fi.attachmentCount = 2;
                fi.pAttachments = views;
                fi.width = width;
                fi.height = height;
                fi.layers = 1;
                ok = check(vkCreateFramebuffer(m->device, &fi, nullptr, &o.fb), "vkCreateFramebuffer", error) &&
                     check(vkMapMemory(m->device, o.readback.memory, 0, VK_WHOLE_SIZE, 0, &o.colourMap), "vkMapMemory", error) &&
                     check(vkMapMemory(m->device, o.depthReadback.memory, 0, VK_WHOLE_SIZE, 0, &o.depthMap), "vkMapMemory", error);
            }
            if (!ok)
            {
                releaseOffscreen();
                return false;
            }
            o.w = width;
            o.h = height;
        }
        Texture &colour = o.colour, &depth = o.depth;
        Buffer &readback = o.readback, &depthReadback = o.depthReadback;
        const VkFramebuffer fb = o.fb;
        ok = ok && m->prepareFrameCopy(draws, width, height, error);
        // the post chain set by setPostEffects (the in-game world: the effects the game left to the native frame) runs over
        // the colour before the readback; the depth read back is the scene's
        const bool post = m->postOn() && !under;
        const bool twoD = std::any_of(draws.begin(), draws.end(), [](const Draw &d) { return d.afterPost; });
        ok = ok && (!post || m->ensurePostBuffers(width, height, error));
        Buffer staging; // `under`, uploaded into the colour target
        if (ok && under)
        {
            ok = under->size() == size_t(width) * height * 4 &&
                 m->createBuffer(under->size(), VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                                 VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, staging, error);
            void *p = nullptr;
            ok = ok && check(vkMapMemory(m->device, staging.memory, 0, VK_WHOLE_SIZE, 0, &p), "vkMapMemory", error);
            if (ok)
            {
                std::memcpy(p, under->data(), under->size());
                vkUnmapMemory(m->device, staging.memory);
            }
            else if (error.empty())
                error = "renderOffscreen: `under` is not width x height RGBA";
        }
        if (!post)
            m->postPrevValid = false;
        if (ok)
        {
            ok = m->oneShot([&](VkCommandBuffer cb) {
                if (under)
                {
                    // the picture into colour, left in the layout the resumed pass expects; depth unused (2D has no Z test)
                    Impl::barrier(cb, colour.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0,
                                  VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
                    VkBufferImageCopy up{};
                    up.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
                    up.imageExtent = {width, height, 1};
                    vkCmdCopyBufferToImage(cb, staging.buffer, colour.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &up);
                    Impl::barrier(cb, colour.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                  VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                                  VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);
                    Impl::barrier(cb, depth.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL, 0,
                                  VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
                                  VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT, VK_IMAGE_ASPECT_DEPTH_BIT);
                }
                else
                    m->recordScene(cb, fb, colour.image, width, height, clip, draws);
                if (post)
                    m->recordPost(cb, colour.image, width, height);
                if (twoD)
                    m->recordScene(cb, fb, colour.image, width, height, clip, draws, true);
                VkBufferImageCopy region{};
                region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
                region.imageExtent = {width, height, 1};
                vkCmdCopyImageToBuffer(cb, colour.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, readback.buffer, 1, &region);
                if (depthOut || gsZOut)
                {
                    Impl::barrier(cb, depth.image, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                  VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT,
                                  VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_IMAGE_ASPECT_DEPTH_BIT);
                    region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
                    vkCmdCopyImageToBuffer(cb, depth.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, depthReadback.buffer, 1, &region);
                }
            }, error);
        }
        m->destroyBuffer(staging); // oneShot waited for the queue
        if (ok && o.cached)
        {
            const VkMappedMemoryRange ranges[2] = {{VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE, nullptr, readback.memory, 0, VK_WHOLE_SIZE},
                                                   {VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE, nullptr, depthReadback.memory, 0, VK_WHOLE_SIZE}};
            vkInvalidateMappedMemoryRanges(m->device, (depthOut || gsZOut) ? 2 : 1, ranges);
        }
        if (ok)
        {
            const uint8_t *p = static_cast<const uint8_t *>(o.colourMap);
            rgbaOut.assign(p, p + size_t(width) * height * 4);
            if (depthOut || gsZOut)
            {
                // stored depth = zScale * (1 - z / w), ~ near / w (0 = nothing drawn); GS Z = 2^23 * depth / zScale
                const float *z = static_cast<const float *>(o.depthMap);
                const size_t n = size_t(width) * height;
                if (depthOut)
                {
                    depthOut->resize(n);
                    for (size_t i = 0; i < n; ++i)
                        (*depthOut)[i] = z[i] > 0.0f ? m->options.nearW / z[i] : 0.0f;
                }
                if (gsZOut)
                {
                    gsZOut->resize(n);
                    const double toGs = 8388608.0 * kGsZNear / m->options.nearW;
                    for (size_t i = 0; i < n; ++i)
                        (*gsZOut)[i] = uint32_t(std::min(16777215.0, std::max(0.0, double(z[i]) * toGs + 0.5)));
                }
            }
        }
        return ok;
    }

    bool Renderer::present(uint32_t windowWidth, uint32_t windowHeight, uint32_t internalWidth, uint32_t internalHeight,
                           const float clip[16], const std::vector<Draw> &draws, std::string &error)
    {
        if (!m->surface)
        {
            error = "present() needs a renderer created with a window surface";
            return false;
        }
        if (!m->frameFence)
        {
            VkFenceCreateInfo fi{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
            fi.flags = VK_FENCE_CREATE_SIGNALED_BIT;
            VkSemaphoreCreateInfo si{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
            VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
            ai.commandPool = m->commandPool;
            ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
            ai.commandBufferCount = 1;
            if (!check(vkCreateFence(m->device, &fi, nullptr, &m->frameFence), "vkCreateFence", error) ||
                !check(vkCreateSemaphore(m->device, &si, nullptr, &m->acquired), "vkCreateSemaphore", error) ||
                !check(vkCreateSemaphore(m->device, &si, nullptr, &m->rendered), "vkCreateSemaphore", error) ||
                !check(vkAllocateCommandBuffers(m->device, &ai, &m->frameCb), "vkAllocateCommandBuffers", error))
                return false;
        }
        vkWaitForFences(m->device, 1, &m->frameFence, VK_TRUE, UINT64_MAX); // one frame in flight
        if (!m->ensureTarget(internalWidth, internalHeight, error) || !m->ensureSwapchain(windowWidth, windowHeight, error) ||
            !m->prepareFrameCopy(draws, internalWidth, internalHeight, error))
            return false;
        if (!m->swapchain)
            return true; // minimised
        uint32_t index = 0;
        VkResult r = vkAcquireNextImageKHR(m->device, m->swapchain, UINT64_MAX, m->acquired, VK_NULL_HANDLE, &index);
        if (r == VK_ERROR_OUT_OF_DATE_KHR)
        {
            m->swapExtent = {}; // recreate on the next call
            return true;
        }
        if (r != VK_SUCCESS && r != VK_SUBOPTIMAL_KHR)
            return check(r, "vkAcquireNextImageKHR", error);
        vkResetFences(m->device, 1, &m->frameFence);
        VkCommandBuffer cb = m->frameCb;
        vkResetCommandBuffer(cb, 0);
        VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vkBeginCommandBuffer(cb, &bi);
        const bool overlay = m->overlayOn && m->overlay.image;
        if (!overlay)
            m->recordScene(cb, m->targetFb, m->targetColour.image, internalWidth, internalHeight, clip, draws);
        if (!overlay && m->postOn())
        {
            if (!m->ensurePostBuffers(internalWidth, internalHeight, error))
                return false;
            m->recordPost(cb, m->targetColour.image, internalWidth, internalHeight);
        }
        else
            m->postPrevValid = false; // a skipped post chain leaves postPrev stale
        if (!overlay && std::any_of(draws.begin(), draws.end(), [](const Draw &d) { return d.afterPost; }))
            m->recordScene(cb, m->targetFb, m->targetColour.image, internalWidth, internalHeight, clip, draws, true);
        VkImage dst = m->swapImages[index];
        Impl::barrier(cb, dst, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT,
                      VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
        VkClearColorValue black{};
        VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkCmdClearColorImage(cb, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &black, 1, &range);
        // the frame scaled to fit the window, aspect kept (bars around it)
        const VkExtent2D e = m->realExtent;
        const uint32_t srcW = overlay ? m->overlayW : internalWidth, srcH = overlay ? m->overlayH : internalHeight;
        const double scale = std::min(double(e.width) / srcW, double(e.height) / srcH);
        const int32_t w = static_cast<int32_t>(srcW * scale), h = static_cast<int32_t>(srcH * scale);
        const int32_t x = (static_cast<int32_t>(e.width) - w) / 2, y = (static_cast<int32_t>(e.height) - h) / 2;
        VkImageBlit blit{};
        blit.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        blit.srcOffsets[1] = {static_cast<int32_t>(srcW), static_cast<int32_t>(srcH), 1};
        blit.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        blit.dstOffsets[0] = {x, y, 0};
        blit.dstOffsets[1] = {x + w, y + h, 1};
        Impl::barrier(cb, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT,
                      VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
        vkCmdBlitImage(cb, overlay ? m->overlay.image : m->targetColour.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dst,
                       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, VK_FILTER_LINEAR);
        Impl::barrier(cb, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_ACCESS_TRANSFER_WRITE_BIT, 0,
                      VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);
        vkEndCommandBuffer(cb);
        const VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
        VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        si.waitSemaphoreCount = 1;
        si.pWaitSemaphores = &m->acquired;
        si.pWaitDstStageMask = &waitStage;
        si.commandBufferCount = 1;
        si.pCommandBuffers = &cb;
        si.signalSemaphoreCount = 1;
        si.pSignalSemaphores = &m->rendered;
        if (!check(vkQueueSubmit(m->queue, 1, &si, m->frameFence), "vkQueueSubmit (frame)", error))
            return false;
        VkPresentInfoKHR pi{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
        pi.waitSemaphoreCount = 1;
        pi.pWaitSemaphores = &m->rendered;
        pi.swapchainCount = 1;
        pi.pSwapchains = &m->swapchain;
        pi.pImageIndices = &index;
        r = vkQueuePresentKHR(m->queue, &pi);
        if (r == VK_ERROR_OUT_OF_DATE_KHR || r == VK_SUBOPTIMAL_KHR)
            m->swapExtent = {}; // recreate on the next call
        else if (r != VK_SUCCESS)
            return check(r, "vkQueuePresentKHR", error);
        return true;
    }
}
