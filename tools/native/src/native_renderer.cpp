// Native renderer core: see native_renderer.h.
#include "native_renderer.h"
#include "shaders_spv.h"

#include <volk.h>

#include <algorithm>
#include <cstring>
#include <memory>
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
        VkInstance instance = VK_NULL_HANDLE;
        VkPhysicalDevice physical = VK_NULL_HANDLE;
        VkPhysicalDeviceProperties props{};
        VkDevice device = VK_NULL_HANDLE;
        uint32_t queueFamily = 0;
        VkQueue queue = VK_NULL_HANDLE;
        VkCommandPool commandPool = VK_NULL_HANDLE;
        VkSampler sampler = VK_NULL_HANDLE;
        VkDescriptorSetLayout setLayout = VK_NULL_HANDLE;
        VkPipelineLayout pipelineLayout = VK_NULL_HANDLE;
        VkDescriptorPool descriptorPool = VK_NULL_HANDLE;
        VkRenderPass renderPass = VK_NULL_HANDLE;
        VkPipeline pipelineOpaque = VK_NULL_HANDLE, pipelineBlend = VK_NULL_HANDLE;
        Buffer vertexBuffer, indexBuffer;

        std::vector<Image> images;
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

        bool uploadBuffer(const void *data, VkDeviceSize size, VkBufferUsageFlags usage, Buffer &out, std::string &error)
        {
            if (!createBuffer(size, usage | VK_BUFFER_USAGE_TRANSFER_DST_BIT, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, out, error))
                return false;
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

        VkShaderModule shader(const uint32_t *code, size_t bytes, std::string &error)
        {
            VkShaderModuleCreateInfo si{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
            si.codeSize = bytes;
            si.pCode = code;
            VkShaderModule m = VK_NULL_HANDLE;
            check(vkCreateShaderModule(device, &si, nullptr, &m), "vkCreateShaderModule", error);
            return m;
        }

        // the scene pass into `fb` (colour ends in TRANSFER_SRC layout, see the render pass)
        void recordScene(VkCommandBuffer cb, VkFramebuffer fb, uint32_t width, uint32_t height, const float clip[16],
                         const std::vector<Draw> &draws)
        {
            // The game's clip matrix is stored for row vectors (clip = [x y z 1] * C), so read as a
            // column-major array it is already C transposed, the matrix for column vectors.
            VkClearValue clear[2];
            std::memcpy(clear[0].color.float32, options.clearColour, sizeof(float) * 4);
            clear[1].depthStencil = {0.0f, 0};
            VkRenderPassBeginInfo rb{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
            rb.renderPass = renderPass;
            rb.framebuffer = fb;
            rb.renderArea.extent = {width, height};
            rb.clearValueCount = 2;
            rb.pClearValues = clear;
            vkCmdBeginRenderPass(cb, &rb, VK_SUBPASS_CONTENTS_INLINE);
            VkViewport vp{0, 0, float(width), float(height), 0.0f, 1.0f};
            VkRect2D sc{{0, 0}, {width, height}};
            vkCmdSetViewport(cb, 0, 1, &vp);
            vkCmdSetScissor(cb, 0, 1, &sc);
            VkDeviceSize zero = 0;
            vkCmdBindVertexBuffers(cb, 0, 1, &vertexBuffer.buffer, &zero);
            vkCmdBindIndexBuffer(cb, indexBuffer.buffer, 0, VK_INDEX_TYPE_UINT32);
            for (int pass = 0; pass < 2; ++pass) // opaque + alpha-tested, then blended in draw order
            {
                vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pass ? pipelineBlend : pipelineOpaque);
                for (const Draw &d : draws)
                {
                    if (d.mesh < 0 || static_cast<size_t>(d.mesh) >= ranges.size())
                        continue;
                    const MeshRange &r = ranges[d.mesh];
                    const Material *mat = r.material >= 0 && static_cast<size_t>(r.material) < materials.size() ? &materials[r.material] : nullptr;
                    const AlphaMode mode = mat ? mat->alphaMode : AlphaMode::Opaque;
                    if ((mode == AlphaMode::Blend) != (pass == 1) || r.indexCount == 0)
                        continue;
                    Push push;
                    mul(clip, d.model, push.mvp);
                    push.params[0] = static_cast<float>(static_cast<int>(mode));
                    push.params[1] = mat ? mat->cutoff : 0.5f;
                    push.params[2] = mat && mat->lightmapImage >= 0 ? 1.0f : 0.0f;
                    push.params[3] = options.nearW;
                    VkDescriptorSet set = mat ? materialSets[r.material] : whiteSet;
                    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLayout, 0, 1, &set, 0, nullptr);
                    vkCmdPushConstants(cb, pipelineLayout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(Push), &push);
                    vkCmdDrawIndexed(cb, r.indexCount, 1, r.firstIndex, r.vertexOffset, 0);
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
            if (!createImage(w, h, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
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
            VkRenderPassCreateInfo rp{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
            rp.attachmentCount = 2;
            rp.pAttachments = at;
            rp.subpassCount = 1;
            rp.pSubpasses = &sub;
            rp.dependencyCount = 1;
            rp.pDependencies = &dep;
            if (!check(vkCreateRenderPass(device, &rp, nullptr, &renderPass), "vkCreateRenderPass", error))
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

            VkShaderModule vs = shader(kMeshVert, sizeof(kMeshVert), error), fs = shader(kMeshFrag, sizeof(kMeshFrag), error);
            if (!vs || !fs)
                return false;
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
            rs.cullMode = VK_CULL_MODE_NONE; // the converter marks all materials double-sided for now
            rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
            rs.lineWidth = 1.0f;
            VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
            ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
            VkPipelineDepthStencilStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
            ds.depthTestEnable = VK_TRUE;
            ds.depthWriteEnable = VK_TRUE;
            ds.depthCompareOp = VK_COMPARE_OP_GREATER; // depth = near / w: nearer is larger
            VkPipelineColorBlendAttachmentState ba{};
            ba.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
            VkPipelineColorBlendStateCreateInfo cbs{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
            cbs.attachmentCount = 1;
            cbs.pAttachments = &ba;
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
            bool ok = check(vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &gp, nullptr, &pipelineOpaque), "opaque pipeline", error);
            // blend: GS ALPHA (Cs - Cd) * As + Cd, depth tested but not written
            ds.depthWriteEnable = VK_FALSE;
            ba.blendEnable = VK_TRUE;
            ba.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
            ba.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
            ba.colorBlendOp = VK_BLEND_OP_ADD;
            ba.srcAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
            ba.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
            ba.alphaBlendOp = VK_BLEND_OP_ADD;
            ok = ok && check(vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &gp, nullptr, &pipelineBlend), "blend pipeline", error);
            vkDestroyShaderModule(device, vs, nullptr);
            vkDestroyShaderModule(device, fs, nullptr);
            return ok;
        }
    };

    Renderer::Renderer() : m(new Impl) {}

    Renderer::~Renderer()
    {
        if (m->device)
        {
            vkDeviceWaitIdle(m->device);
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
            for (Texture &t : m->textures)
                m->destroyImage(t);
            if (m->descriptorPool)
                vkDestroyDescriptorPool(m->device, m->descriptorPool, nullptr);
            if (m->pipelineOpaque)
                vkDestroyPipeline(m->device, m->pipelineOpaque, nullptr);
            if (m->pipelineBlend)
                vkDestroyPipeline(m->device, m->pipelineBlend, nullptr);
            if (m->pipelineLayout)
                vkDestroyPipelineLayout(m->device, m->pipelineLayout, nullptr);
            if (m->setLayout)
                vkDestroyDescriptorSetLayout(m->device, m->setLayout, nullptr);
            if (m->renderPass)
                vkDestroyRenderPass(m->device, m->renderPass, nullptr);
            if (m->sampler)
                vkDestroySampler(m->device, m->sampler, nullptr);
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
        return m->createPipelines(error);
    }

    int Renderer::addImage(const Image &image)
    {
        m->images.push_back(image);
        return static_cast<int>(m->images.size()) - 1;
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

    bool Renderer::upload(std::string &error)
    {
        // textures: the images, then a 1x1 white one for missing textures / no lightmap
        Image white{1, 1, {255, 255, 255, 255}};
        m->textures.resize(m->images.size() + 1);
        for (size_t i = 0; i <= m->images.size(); ++i)
            if (!m->uploadTexture(i < m->images.size() ? m->images[i] : white, m->textures[i], error))
                return false;
        const size_t whiteIndex = m->images.size();
        m->images.clear();
        m->images.shrink_to_fit();

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
        auto texView = [&](int image) { return m->textures[image >= 0 && static_cast<size_t>(image) < whiteIndex ? image : whiteIndex].view; };
        for (uint32_t s = 0; s < sets; ++s)
        {
            const Material *mat = s < m->materials.size() ? &m->materials[s] : nullptr;
            VkDescriptorImageInfo info[2] = {
                {m->sampler, texView(mat ? mat->baseImage : -1), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
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
               m->uploadBuffer(idx.data(), idx.size() * sizeof(uint32_t), VK_BUFFER_USAGE_INDEX_BUFFER_BIT, m->indexBuffer, error);
    }

    bool Renderer::renderOffscreen(uint32_t width, uint32_t height, const float clip[16], const std::vector<Draw> &draws,
                                   std::vector<uint8_t> &rgbaOut, std::string &error, std::vector<float> *depthOut)
    {
        Texture colour, depth;
        Buffer readback, depthReadback;
        VkFramebuffer fb = VK_NULL_HANDLE;
        bool ok = m->createImage(width, height, VK_FORMAT_R8G8B8A8_UNORM,
                                 VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, VK_IMAGE_ASPECT_COLOR_BIT, colour, error) &&
                  m->createImage(width, height, VK_FORMAT_D32_SFLOAT,
                                 VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                                 VK_IMAGE_ASPECT_DEPTH_BIT, depth, error) &&
                  m->createBuffer(VkDeviceSize(width) * height * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                  VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, depthReadback, error) &&
                  m->createBuffer(VkDeviceSize(width) * height * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                  VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, readback, error);
        if (ok)
        {
            VkImageView views[2] = {colour.view, depth.view};
            VkFramebufferCreateInfo fi{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
            fi.renderPass = m->renderPass;
            fi.attachmentCount = 2;
            fi.pAttachments = views;
            fi.width = width;
            fi.height = height;
            fi.layers = 1;
            ok = check(vkCreateFramebuffer(m->device, &fi, nullptr, &fb), "vkCreateFramebuffer", error);
        }
        if (ok)
        {
            ok = m->oneShot([&](VkCommandBuffer cb) {
                m->recordScene(cb, fb, width, height, clip, draws);
                VkBufferImageCopy region{};
                region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
                region.imageExtent = {width, height, 1};
                vkCmdCopyImageToBuffer(cb, colour.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, readback.buffer, 1, &region);
                if (depthOut)
                {
                    Impl::barrier(cb, depth.image, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                  VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT,
                                  VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_IMAGE_ASPECT_DEPTH_BIT);
                    region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
                    vkCmdCopyImageToBuffer(cb, depth.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, depthReadback.buffer, 1, &region);
                }
            }, error);
        }
        if (ok)
        {
            void *p;
            vkMapMemory(m->device, readback.memory, 0, readback.size, 0, &p);
            rgbaOut.assign(static_cast<uint8_t *>(p), static_cast<uint8_t *>(p) + size_t(width) * height * 4);
            vkUnmapMemory(m->device, readback.memory);
            if (depthOut)
            {
                // stored depth = near / w (0 = nothing drawn)
                vkMapMemory(m->device, depthReadback.memory, 0, depthReadback.size, 0, &p);
                const float *z = static_cast<const float *>(p);
                depthOut->resize(size_t(width) * height);
                for (size_t i = 0; i < depthOut->size(); ++i)
                    (*depthOut)[i] = z[i] > 0.0f ? m->options.nearW / z[i] : 0.0f;
                vkUnmapMemory(m->device, depthReadback.memory);
            }
        }
        if (fb)
            vkDestroyFramebuffer(m->device, fb, nullptr);
        m->destroyImage(colour);
        m->destroyImage(depth);
        m->destroyBuffer(readback);
        m->destroyBuffer(depthReadback);
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
        if (!m->ensureTarget(internalWidth, internalHeight, error) || !m->ensureSwapchain(windowWidth, windowHeight, error))
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
        m->recordScene(cb, m->targetFb, internalWidth, internalHeight, clip, draws);
        VkImage dst = m->swapImages[index];
        Impl::barrier(cb, dst, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT,
                      VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
        VkClearColorValue black{};
        VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkCmdClearColorImage(cb, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &black, 1, &range);
        // the frame scaled to fit the window, aspect kept (bars around it)
        const VkExtent2D e = m->realExtent;
        const double scale = std::min(double(e.width) / internalWidth, double(e.height) / internalHeight);
        const int32_t w = static_cast<int32_t>(internalWidth * scale), h = static_cast<int32_t>(internalHeight * scale);
        const int32_t x = (static_cast<int32_t>(e.width) - w) / 2, y = (static_cast<int32_t>(e.height) - h) / 2;
        VkImageBlit blit{};
        blit.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        blit.srcOffsets[1] = {static_cast<int32_t>(internalWidth), static_cast<int32_t>(internalHeight), 1};
        blit.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        blit.dstOffsets[0] = {x, y, 0};
        blit.dstOffsets[1] = {x + w, y + h, 1};
        Impl::barrier(cb, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT,
                      VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
        vkCmdBlitImage(cb, m->targetColour.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1,
                       &blit, VK_FILTER_LINEAR);
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
