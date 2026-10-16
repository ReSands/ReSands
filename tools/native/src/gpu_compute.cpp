#include "gpu_compute.h"

#include <algorithm>
#include <cstring>
#include <fstream>

#include <volk.h>
#if defined(__APPLE__)
#include <dlfcn.h>
#endif

namespace pop_gpu
{
    struct Buffer
    {
        VkBuffer buffer = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;
        void *mapped = nullptr;
        size_t bytes = 0;
    };
    struct Pipeline
    {
        VkDescriptorSetLayout setLayout = VK_NULL_HANDLE;
        VkPipelineLayout layout = VK_NULL_HANDLE;
        VkPipeline pipeline = VK_NULL_HANDLE;
        int bindings = 0;
    };

    struct Compute::Impl
    {
        VkInstance instance = VK_NULL_HANDLE;
        VkPhysicalDevice physical = VK_NULL_HANDLE;
        VkPhysicalDeviceProperties props{};
        VkDevice device = VK_NULL_HANDLE;
        VkQueue queue = VK_NULL_HANDLE;
        uint32_t family = 0;
        VkCommandPool pool = VK_NULL_HANDLE;
        VkCommandBuffer cmd = VK_NULL_HANDLE;
        VkFence fence = VK_NULL_HANDLE;
        VkDescriptorPool descriptors = VK_NULL_HANDLE;
        std::vector<Buffer> buffers;
        std::vector<Pipeline> pipelines;

        ~Impl()
        {
            if (!device)
            {
                if (instance)
                    vkDestroyInstance(instance, nullptr);
                return;
            }
            vkDeviceWaitIdle(device);
            for (Buffer &b : buffers)
            {
                vkDestroyBuffer(device, b.buffer, nullptr);
                vkFreeMemory(device, b.memory, nullptr);
            }
            for (Pipeline &p : pipelines)
            {
                vkDestroyPipeline(device, p.pipeline, nullptr);
                vkDestroyPipelineLayout(device, p.layout, nullptr);
                vkDestroyDescriptorSetLayout(device, p.setLayout, nullptr);
            }
            if (descriptors)
                vkDestroyDescriptorPool(device, descriptors, nullptr);
            if (fence)
                vkDestroyFence(device, fence, nullptr);
            if (pool)
                vkDestroyCommandPool(device, pool, nullptr);
            vkDestroyDevice(device, nullptr);
            vkDestroyInstance(instance, nullptr);
        }
    };

    namespace
    {
        bool check(VkResult r, const char *what, std::string &error)
        {
            if (r == VK_SUCCESS)
                return true;
            error = std::string(what) + " failed (VkResult " + std::to_string(int(r)) + ")";
            return false;
        }
    }

    Compute::Compute() : m(new Impl) {}
    Compute::~Compute() = default;

    std::string Compute::deviceName() const { return m->props.deviceName; }

    bool Compute::init(std::string &error)
    {
        VkResult loaded = volkInitialize();
#if defined(__APPLE__)
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
        uint32_t extCount = 0;
        vkEnumerateInstanceExtensionProperties(nullptr, &extCount, nullptr);
        std::vector<VkExtensionProperties> exts(extCount);
        vkEnumerateInstanceExtensionProperties(nullptr, &extCount, exts.data());
        bool portability = false;
        for (const auto &e : exts)
            portability |= std::strcmp(e.extensionName, VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME) == 0;
        const char *portName = VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME;
        VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
        app.pApplicationName = "pop_gpu_compute";
        app.apiVersion = VK_API_VERSION_1_1;
        VkInstanceCreateInfo ii{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
        ii.pApplicationInfo = &app;
        if (portability)
        {
            ii.enabledExtensionCount = 1;
            ii.ppEnabledExtensionNames = &portName;
            ii.flags |= VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR;
        }
        if (!check(vkCreateInstance(&ii, nullptr, &m->instance), "vkCreateInstance", error))
            return false;
        volkLoadInstance(m->instance);

        uint32_t n = 0;
        vkEnumeratePhysicalDevices(m->instance, &n, nullptr);
        std::vector<VkPhysicalDevice> devs(n);
        vkEnumeratePhysicalDevices(m->instance, &n, devs.data());
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
                if (qs[q].queueFlags & VK_QUEUE_COMPUTE_BIT)
                {
                    if (best < 0 || p.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU)
                    {
                        best = int(i);
                        m->family = q;
                    }
                    break;
                }
        }
        if (best < 0)
        {
            error = "no Vulkan device with a compute queue";
            return false;
        }
        m->physical = devs[size_t(best)];
        vkGetPhysicalDeviceProperties(m->physical, &m->props);
        uint32_t dn = 0;
        vkEnumerateDeviceExtensionProperties(m->physical, nullptr, &dn, nullptr);
        std::vector<VkExtensionProperties> dexts(dn);
        vkEnumerateDeviceExtensionProperties(m->physical, nullptr, &dn, dexts.data());
        std::vector<const char *> denable;
        for (const auto &e : dexts)
            if (std::strcmp(e.extensionName, "VK_KHR_portability_subset") == 0)
                denable.push_back("VK_KHR_portability_subset");
        const float prio = 1.0f;
        VkDeviceQueueCreateInfo qi{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
        qi.queueFamilyIndex = m->family;
        qi.queueCount = 1;
        qi.pQueuePriorities = &prio;
        VkDeviceCreateInfo di{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
        di.queueCreateInfoCount = 1;
        di.pQueueCreateInfos = &qi;
        di.enabledExtensionCount = uint32_t(denable.size());
        di.ppEnabledExtensionNames = denable.data();
        if (!check(vkCreateDevice(m->physical, &di, nullptr, &m->device), "vkCreateDevice", error))
            return false;
        volkLoadDevice(m->device);
        vkGetDeviceQueue(m->device, m->family, 0, &m->queue);
        VkCommandPoolCreateInfo cp{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        cp.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        cp.queueFamilyIndex = m->family;
        if (!check(vkCreateCommandPool(m->device, &cp, nullptr, &m->pool), "vkCreateCommandPool", error))
            return false;
        VkCommandBufferAllocateInfo ca{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        ca.commandPool = m->pool;
        ca.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        ca.commandBufferCount = 1;
        if (!check(vkAllocateCommandBuffers(m->device, &ca, &m->cmd), "vkAllocateCommandBuffers", error))
            return false;
        VkFenceCreateInfo fi{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        if (!check(vkCreateFence(m->device, &fi, nullptr, &m->fence), "vkCreateFence", error))
            return false;
        VkDescriptorPoolSize ps{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 64};
        VkDescriptorPoolCreateInfo dp{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        dp.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
        dp.maxSets = 16;
        dp.poolSizeCount = 1;
        dp.pPoolSizes = &ps;
        return check(vkCreateDescriptorPool(m->device, &dp, nullptr, &m->descriptors), "vkCreateDescriptorPool", error);
    }

    int Compute::createBuffer(size_t bytes)
    {
        Buffer b;
        b.bytes = bytes;
        VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        bi.size = std::max<size_t>(bytes, 16);
        bi.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
        bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        if (vkCreateBuffer(m->device, &bi, nullptr, &b.buffer) != VK_SUCCESS)
            return -1;
        VkMemoryRequirements req;
        vkGetBufferMemoryRequirements(m->device, b.buffer, &req);
        VkPhysicalDeviceMemoryProperties mp;
        vkGetPhysicalDeviceMemoryProperties(m->physical, &mp);
        const VkMemoryPropertyFlags want = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
        uint32_t type = UINT32_MAX;
        for (uint32_t i = 0; i < mp.memoryTypeCount; ++i)
            if ((req.memoryTypeBits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & want) == want)
            {
                type = i;
                break;
            }
        if (type == UINT32_MAX)
            return -1;
        VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        ai.allocationSize = req.size;
        ai.memoryTypeIndex = type;
        if (vkAllocateMemory(m->device, &ai, nullptr, &b.memory) != VK_SUCCESS)
            return -1;
        vkBindBufferMemory(m->device, b.buffer, b.memory, 0);
        vkMapMemory(m->device, b.memory, 0, VK_WHOLE_SIZE, 0, &b.mapped);
        std::memset(b.mapped, 0, bytes);
        m->buffers.push_back(b);
        return int(m->buffers.size()) - 1;
    }

    void *Compute::data(int buffer) { return m->buffers[size_t(buffer)].mapped; }
    size_t Compute::size(int buffer) const { return m->buffers[size_t(buffer)].bytes; }

    int Compute::createPipeline(const std::vector<uint32_t> &spirv, int bindings, std::string &error)
    {
        Pipeline p;
        p.bindings = bindings;
        std::vector<VkDescriptorSetLayoutBinding> b(static_cast<size_t>(bindings));
        for (int i = 0; i < bindings; ++i)
        {
            b[size_t(i)].binding = uint32_t(i);
            b[size_t(i)].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            b[size_t(i)].descriptorCount = 1;
            b[size_t(i)].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        }
        VkDescriptorSetLayoutCreateInfo sl{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        sl.bindingCount = uint32_t(bindings);
        sl.pBindings = b.data();
        if (!check(vkCreateDescriptorSetLayout(m->device, &sl, nullptr, &p.setLayout), "vkCreateDescriptorSetLayout", error))
            return -1;
        VkPushConstantRange pr{VK_SHADER_STAGE_COMPUTE_BIT, 0, 128};
        VkPipelineLayoutCreateInfo pl{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        pl.setLayoutCount = 1;
        pl.pSetLayouts = &p.setLayout;
        pl.pushConstantRangeCount = 1;
        pl.pPushConstantRanges = &pr;
        if (!check(vkCreatePipelineLayout(m->device, &pl, nullptr, &p.layout), "vkCreatePipelineLayout", error))
            return -1;
        VkShaderModuleCreateInfo smi{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        smi.codeSize = spirv.size() * 4;
        smi.pCode = spirv.data();
        VkShaderModule module = VK_NULL_HANDLE;
        if (!check(vkCreateShaderModule(m->device, &smi, nullptr, &module), "vkCreateShaderModule", error))
            return -1;
        VkComputePipelineCreateInfo ci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
        ci.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        ci.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
        ci.stage.module = module;
        ci.stage.pName = "main";
        ci.layout = p.layout;
        const VkResult r = vkCreateComputePipelines(m->device, VK_NULL_HANDLE, 1, &ci, nullptr, &p.pipeline);
        vkDestroyShaderModule(m->device, module, nullptr);
        if (!check(r, "vkCreateComputePipelines", error))
            return -1;
        m->pipelines.push_back(p);
        return int(m->pipelines.size()) - 1;
    }

    bool Compute::dispatch(int pipeline, const std::vector<int> &buffers, const void *push, size_t pushBytes, uint32_t gx, uint32_t gy,
                           uint32_t gz, std::string &error)
    {
        const Pipeline &p = m->pipelines[size_t(pipeline)];
        if (int(buffers.size()) != p.bindings || pushBytes > 128)
        {
            error = "dispatch: wrong buffer count or push constants over 128 bytes";
            return false;
        }
        VkDescriptorSetAllocateInfo da{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        da.descriptorPool = m->descriptors;
        da.descriptorSetCount = 1;
        da.pSetLayouts = &p.setLayout;
        VkDescriptorSet set = VK_NULL_HANDLE;
        if (!check(vkAllocateDescriptorSets(m->device, &da, &set), "vkAllocateDescriptorSets", error))
            return false;
        std::vector<VkDescriptorBufferInfo> infos(buffers.size());
        std::vector<VkWriteDescriptorSet> writes(buffers.size());
        for (size_t i = 0; i < buffers.size(); ++i)
        {
            infos[i] = {m->buffers[size_t(buffers[i])].buffer, 0, VK_WHOLE_SIZE};
            writes[i] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
            writes[i].dstSet = set;
            writes[i].dstBinding = uint32_t(i);
            writes[i].descriptorCount = 1;
            writes[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            writes[i].pBufferInfo = &infos[i];
        }
        vkUpdateDescriptorSets(m->device, uint32_t(writes.size()), writes.data(), 0, nullptr);
        vkResetCommandBuffer(m->cmd, 0);
        VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vkBeginCommandBuffer(m->cmd, &bi);
        vkCmdBindPipeline(m->cmd, VK_PIPELINE_BIND_POINT_COMPUTE, p.pipeline);
        vkCmdBindDescriptorSets(m->cmd, VK_PIPELINE_BIND_POINT_COMPUTE, p.layout, 0, 1, &set, 0, nullptr);
        uint8_t pc[128] = {};
        if (push && pushBytes)
            std::memcpy(pc, push, pushBytes);
        vkCmdPushConstants(m->cmd, p.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, 128, pc);
        vkCmdDispatch(m->cmd, gx, gy, gz);
        VkMemoryBarrier mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
        mb.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        mb.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
        vkCmdPipelineBarrier(m->cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &mb, 0, nullptr, 0, nullptr);
        vkEndCommandBuffer(m->cmd);
        VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        si.commandBufferCount = 1;
        si.pCommandBuffers = &m->cmd;
        vkResetFences(m->device, 1, &m->fence);
        bool ok = check(vkQueueSubmit(m->queue, 1, &si, m->fence), "vkQueueSubmit", error) &&
                  check(vkWaitForFences(m->device, 1, &m->fence, VK_TRUE, 30ull * 1000 * 1000 * 1000), "vkWaitForFences (GPU timeout?)", error);
        vkFreeDescriptorSets(m->device, m->descriptors, 1, &set);
        return ok;
    }

    bool readSpirv(const std::string &path, std::vector<uint32_t> &words)
    {
        std::ifstream f(path, std::ios::binary | std::ios::ate);
        if (!f)
            return false;
        const std::streamsize n = f.tellg();
        if (n <= 0 || n % 4)
            return false;
        words.resize(size_t(n) / 4);
        f.seekg(0);
        return bool(f.read(reinterpret_cast<char *>(words.data()), n));
    }
}
