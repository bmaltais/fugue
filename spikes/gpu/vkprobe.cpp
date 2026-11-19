// Spike 04, part 2: raw Vulkan, no window. Lists devices and memory heaps, reads
// VK_EXT_memory_budget, and runs one compute dispatch on each device.
#include <vulkan/vulkan.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unistd.h>
#include <vector>
static const uint32_t kSpv[] =
#include "square.spv.inc"  // glslc -mfmt=c emits the braces
    ;
#define CK(x) do { VkResult r_ = (x); if (r_ != VK_SUCCESS) { printf("FAIL %s = %d (line %d)\n", #x, r_, __LINE__); return false; } } while (0)

static const char *typeName(VkPhysicalDeviceType t) {
    switch (t) {
    case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU: return "discrete";
    case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU: return "integrated";
    case VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU: return "virtual";
    case VK_PHYSICAL_DEVICE_TYPE_CPU: return "cpu";
    default: return "other";
    }
}
static double mib(VkDeviceSize b) { return b / 1048576.0; }

static bool runCompute(VkPhysicalDevice pd, uint32_t queueFamily) {
    float prio = 1.0f;
    VkDeviceQueueCreateInfo qci{}; qci.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    qci.queueFamilyIndex = queueFamily; qci.queueCount = 1; qci.pQueuePriorities = &prio;
    VkDeviceCreateInfo dci{}; dci.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    dci.queueCreateInfoCount = 1; dci.pQueueCreateInfos = &qci;
    VkDevice dev;
    CK(vkCreateDevice(pd, &dci, nullptr, &dev));
    VkQueue q; vkGetDeviceQueue(dev, queueFamily, 0, &q);

    const uint32_t N = 1u << 20;
    VkBufferCreateInfo bci{}; bci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bci.size = N * 4; bci.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT; bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    VkBuffer buf; CK(vkCreateBuffer(dev, &bci, nullptr, &buf));
    VkMemoryRequirements mr; vkGetBufferMemoryRequirements(dev, buf, &mr);
    VkPhysicalDeviceMemoryProperties mp; vkGetPhysicalDeviceMemoryProperties(pd, &mp);
    uint32_t mt = ~0u;
    for (uint32_t i = 0; i < mp.memoryTypeCount; i++)
        if ((mr.memoryTypeBits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) == (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) { mt = i; break; }
    if (mt == ~0u) { printf("no host-visible memory type\n"); return false; }
    VkMemoryAllocateInfo mai{}; mai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    mai.allocationSize = mr.size; mai.memoryTypeIndex = mt;
    VkDeviceMemory mem; CK(vkAllocateMemory(dev, &mai, nullptr, &mem));
    CK(vkBindBufferMemory(dev, buf, mem, 0));

    VkShaderModuleCreateInfo smci{}; smci.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    smci.codeSize = sizeof kSpv; smci.pCode = kSpv;
    VkShaderModule sm; CK(vkCreateShaderModule(dev, &smci, nullptr, &sm));
    VkDescriptorSetLayoutBinding b{0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
    VkDescriptorSetLayoutCreateInfo dslci{}; dslci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    dslci.bindingCount = 1; dslci.pBindings = &b;
    VkDescriptorSetLayout dsl; CK(vkCreateDescriptorSetLayout(dev, &dslci, nullptr, &dsl));
    VkPipelineLayoutCreateInfo plci{}; plci.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    plci.setLayoutCount = 1; plci.pSetLayouts = &dsl;
    VkPipelineLayout pl; CK(vkCreatePipelineLayout(dev, &plci, nullptr, &pl));
    VkComputePipelineCreateInfo cpci{}; cpci.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    cpci.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_COMPUTE_BIT, sm, "main", nullptr};
    cpci.layout = pl;
    VkPipeline pipe; CK(vkCreateComputePipelines(dev, VK_NULL_HANDLE, 1, &cpci, nullptr, &pipe));
    VkDescriptorPoolSize ps{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1};
    VkDescriptorPoolCreateInfo dpci{}; dpci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    dpci.maxSets = 1; dpci.poolSizeCount = 1; dpci.pPoolSizes = &ps;
    VkDescriptorPool dp; CK(vkCreateDescriptorPool(dev, &dpci, nullptr, &dp));
    VkDescriptorSetAllocateInfo dsai{}; dsai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    dsai.descriptorPool = dp; dsai.descriptorSetCount = 1; dsai.pSetLayouts = &dsl;
    VkDescriptorSet ds; CK(vkAllocateDescriptorSets(dev, &dsai, &ds));
    VkDescriptorBufferInfo dbi{buf, 0, VK_WHOLE_SIZE};
    VkWriteDescriptorSet w{}; w.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    w.dstSet = ds; w.descriptorCount = 1; w.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; w.pBufferInfo = &dbi;
    vkUpdateDescriptorSets(dev, 1, &w, 0, nullptr);

    VkCommandPoolCreateInfo cpi{}; cpi.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO; cpi.queueFamilyIndex = queueFamily;
    VkCommandPool cp; CK(vkCreateCommandPool(dev, &cpi, nullptr, &cp));
    VkCommandBufferAllocateInfo cbai{}; cbai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    cbai.commandPool = cp; cbai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; cbai.commandBufferCount = 1;
    VkCommandBuffer cb; CK(vkAllocateCommandBuffers(dev, &cbai, &cb));
    VkCommandBufferBeginInfo bi{}; bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    CK(vkBeginCommandBuffer(cb, &bi));
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pipe);
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pl, 0, 1, &ds, 0, nullptr);
    vkCmdDispatch(cb, N / 256, 1, 1);
    CK(vkEndCommandBuffer(cb));
    VkSubmitInfo si{}; si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO; si.commandBufferCount = 1; si.pCommandBuffers = &cb;
    CK(vkQueueSubmit(q, 1, &si, VK_NULL_HANDLE));
    CK(vkQueueWaitIdle(q));
    void *p; CK(vkMapMemory(dev, mem, 0, VK_WHOLE_SIZE, 0, &p));
    const uint32_t *v = (const uint32_t *)p; uint32_t bad = 0;
    for (uint32_t i = 0; i < N; i++) if (v[i] != i * 2u + 1u) bad++;
    printf("  compute dispatch (1M uints, no window): %s\n", bad ? "WRONG RESULT" : "ok");
    vkUnmapMemory(dev, mem);
    vkDestroyDevice(dev, nullptr);  // frees everything created from it for this probe
    return bad == 0;
}

// --hold MiB: allocate that much device-local memory on the first device and sleep 20 s,
// so a second run can check that the budget sees another process's use.
static int hold(VkInstance inst, VkPhysicalDevice pd, uint32_t mibs) {
    (void)inst;
    float prio = 1.0f;
    VkDeviceQueueCreateInfo qci{}; qci.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    qci.queueCount = 1; qci.pQueuePriorities = &prio;
    VkDeviceCreateInfo dci{}; dci.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    dci.queueCreateInfoCount = 1; dci.pQueueCreateInfos = &qci;
    VkDevice dev; if (vkCreateDevice(pd, &dci, nullptr, &dev) != VK_SUCCESS) return 1;
    VkPhysicalDeviceMemoryProperties mp; vkGetPhysicalDeviceMemoryProperties(pd, &mp);
    // Prefer device-local and host-visible (so the pages can be touched), else device-local.
    uint32_t mt = ~0u;
    const VkMemoryPropertyFlags both = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT | VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT;
    for (uint32_t i = 0; i < mp.memoryTypeCount && mt == ~0u; i++) if ((mp.memoryTypes[i].propertyFlags & both) == both) mt = i;
    for (uint32_t i = 0; i < mp.memoryTypeCount && mt == ~0u; i++) if (mp.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) mt = i;
    if (mt == ~0u) return 1;
    std::vector<VkDeviceMemory> blocks;
    for (uint32_t done = 0; done < mibs; done += 256) {
        VkMemoryAllocateInfo mai{}; mai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        mai.allocationSize = 256ull << 20; mai.memoryTypeIndex = mt;
        VkDeviceMemory m; if (vkAllocateMemory(dev, &mai, nullptr, &m) != VK_SUCCESS) { printf("alloc failed at %u MiB\n", done); break; }
        // Touch the pages where the memory is host-visible: some drivers commit lazily.
        void *p = nullptr;
        if ((mp.memoryTypes[mt].propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) && vkMapMemory(dev, m, 0, VK_WHOLE_SIZE, 0, &p) == VK_SUCCESS) {
            memset(p, 1, mai.allocationSize);
            vkUnmapMemory(dev, m);
        }
        blocks.push_back(m);
    }
    printf("holding %zu MiB (memory type %u%s)\n", blocks.size() * 256, mt,
           (mp.memoryTypes[mt].propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) ? ", host-visible, pages touched" : "");
    fflush(stdout);
    sleep(20);
    vkDestroyDevice(dev, nullptr);
    return 0;
}

int main(int argc, char **argv) {
    VkApplicationInfo ai{}; ai.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO; ai.apiVersion = VK_API_VERSION_1_1;
    VkInstanceCreateInfo ici{}; ici.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO; ici.pApplicationInfo = &ai;
    VkInstance inst;
    if (vkCreateInstance(&ici, nullptr, &inst) != VK_SUCCESS) { printf("no Vulkan instance\n"); return 1; }
    uint32_t n = 0; vkEnumeratePhysicalDevices(inst, &n, nullptr);
    std::vector<VkPhysicalDevice> devs(n); vkEnumeratePhysicalDevices(inst, &n, devs.data());
    if (argc == 3 && !strcmp(argv[1], "--hold") && n) return hold(inst, devs[0], atoi(argv[2]));
    printf("%u Vulkan device(s)\n", n);
    int fails = 0;
    for (auto pd : devs) {
        VkPhysicalDeviceProperties props; vkGetPhysicalDeviceProperties(pd, &props);
        uint32_t ne = 0; vkEnumerateDeviceExtensionProperties(pd, nullptr, &ne, nullptr);
        std::vector<VkExtensionProperties> ex(ne); vkEnumerateDeviceExtensionProperties(pd, nullptr, &ne, ex.data());
        bool budget = false;
        for (auto &e : ex) if (!strcmp(e.extensionName, VK_EXT_MEMORY_BUDGET_EXTENSION_NAME)) budget = true;
        printf("\n%s: %s, vendor 0x%04x, api %u.%u.%u, driver %u\n", props.deviceName, typeName(props.deviceType), props.vendorID,
               VK_VERSION_MAJOR(props.apiVersion), VK_VERSION_MINOR(props.apiVersion), VK_VERSION_PATCH(props.apiVersion), props.driverVersion);
        VkPhysicalDeviceMemoryBudgetPropertiesEXT bp{}; bp.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_BUDGET_PROPERTIES_EXT;
        VkPhysicalDeviceMemoryProperties2 mp2{}; mp2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_PROPERTIES_2;
        if (budget) mp2.pNext = &bp;
        auto fn = (PFN_vkGetPhysicalDeviceMemoryProperties2)vkGetInstanceProcAddr(inst, "vkGetPhysicalDeviceMemoryProperties2");
        fn(pd, &mp2);
        printf("  VK_EXT_memory_budget: %s\n", budget ? "yes" : "no");
        for (uint32_t i = 0; i < mp2.memoryProperties.memoryHeapCount; i++) {
            auto &h = mp2.memoryProperties.memoryHeaps[i];
            printf("  heap %u: %s%.0f MiB", i, (h.flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) ? "device-local " : "host ", mib(h.size));
            if (budget) printf("  budget %.0f MiB  usage %.0f MiB  (headroom %.0f MiB)", mib(bp.heapBudget[i]), mib(bp.heapUsage[i]), mib(bp.heapBudget[i]) - mib(bp.heapUsage[i]));
            printf("\n");
        }
        uint32_t nq = 0; vkGetPhysicalDeviceQueueFamilyProperties(pd, &nq, nullptr);
        std::vector<VkQueueFamilyProperties> qf(nq); vkGetPhysicalDeviceQueueFamilyProperties(pd, &nq, qf.data());
        uint32_t fam = ~0u;
        for (uint32_t i = 0; i < nq; i++) if (qf[i].queueFlags & VK_QUEUE_COMPUTE_BIT) { fam = i; break; }
        if (fam == ~0u) { printf("  no compute queue\n"); fails++; continue; }
        if (!runCompute(pd, fam)) fails++;
    }
    vkDestroyInstance(inst, nullptr);
    return fails;
}
