#include "vk/scope_layer.hpp"
#include "se4/log.hpp"
#include "vk/queue_lock.hpp"
#include "xr/runtime.hpp"
#include <windows.h>

namespace scopelayer {
namespace {
const uint32_t kScopeSpv[] =
#include "scope.comp.inc"
    ;

struct Fn {
    VkDevice dev = VK_NULL_HANDLE;
    VkPhysicalDeviceMemoryProperties mem{};
    PFN_vkCreateImage createImage;
    PFN_vkDestroyImage destroyImage;
    PFN_vkCreateImageView createView;
    PFN_vkDestroyImageView destroyView;
    PFN_vkGetImageMemoryRequirements memReq;
    PFN_vkAllocateMemory alloc;
    PFN_vkFreeMemory freeMem;
    PFN_vkBindImageMemory bind;
    PFN_vkCreateDescriptorSetLayout createDsl;
    PFN_vkCreatePipelineLayout createPl;
    PFN_vkCreateShaderModule createShader;
    PFN_vkDestroyShaderModule destroyShader;
    PFN_vkCreateComputePipelines createCompute;
    PFN_vkCreateDescriptorPool createPool;
    PFN_vkAllocateDescriptorSets allocSets;
    PFN_vkUpdateDescriptorSets updateSets;
    PFN_vkCmdPipelineBarrier barrier;
    PFN_vkCmdBindPipeline bindPipeline;
    PFN_vkCmdBindDescriptorSets bindSets;
    PFN_vkCmdPushConstants push;
    PFN_vkCmdDispatch dispatch;
    PFN_vkCmdCopyImage copy;
    PFN_vkDeviceWaitIdle waitIdle;
} vk;

xr::Swapchain g_sc;
bool g_failed;
uint32_t g_size;
VkFormat g_srcFormat = VK_FORMAT_UNDEFINED;
uint32_t g_index;
bool g_acquired;

VkImage g_work = VK_NULL_HANDLE;  // RGBA8_UNORM copy of the engine image, masked in place
VkImageView g_workView = VK_NULL_HANDLE;
VkDeviceMemory g_workMem = VK_NULL_HANDLE;
VkDescriptorSetLayout g_dsl;
VkPipelineLayout g_pl;
VkPipeline g_pipe;
VkDescriptorPool g_pool;
VkDescriptorSet g_set;

uint32_t memType(uint32_t bits) {
    for (uint32_t i = 0; i < vk.mem.memoryTypeCount; ++i)
        if ((bits & (1u << i)) && (vk.mem.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) return i;
    return UINT32_MAX;
}

bool createPipeline() {
    VkDescriptorSetLayoutBinding b{0, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
    VkDescriptorSetLayoutCreateInfo dl{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    dl.bindingCount = 1;
    dl.pBindings = &b;
    if (vk.createDsl(vk.dev, &dl, nullptr, &g_dsl) != VK_SUCCESS) return false;
    VkPushConstantRange pr{VK_SHADER_STAGE_COMPUTE_BIT, 0, 16};
    VkPipelineLayoutCreateInfo pl{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pl.setLayoutCount = 1;
    pl.pSetLayouts = &g_dsl;
    pl.pushConstantRangeCount = 1;
    pl.pPushConstantRanges = &pr;
    if (vk.createPl(vk.dev, &pl, nullptr, &g_pl) != VK_SUCCESS) return false;
    VkShaderModuleCreateInfo sm{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    sm.codeSize = sizeof(kScopeSpv);
    sm.pCode = kScopeSpv;
    VkShaderModule mod;
    if (vk.createShader(vk.dev, &sm, nullptr, &mod) != VK_SUCCESS) return false;
    VkComputePipelineCreateInfo cp{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    cp.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_COMPUTE_BIT, mod, "main", nullptr};
    cp.layout = g_pl;
    VkResult r = vk.createCompute(vk.dev, VK_NULL_HANDLE, 1, &cp, nullptr, &g_pipe);
    vk.destroyShader(vk.dev, mod, nullptr);
    if (r != VK_SUCCESS) return false;
    VkDescriptorPoolSize ps{VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1};
    VkDescriptorPoolCreateInfo dp{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    dp.maxSets = 1;
    dp.poolSizeCount = 1;
    dp.pPoolSizes = &ps;
    if (vk.createPool(vk.dev, &dp, nullptr, &g_pool) != VK_SUCCESS) return false;
    VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    ai.descriptorPool = g_pool;
    ai.descriptorSetCount = 1;
    ai.pSetLayouts = &g_dsl;
    return vk.allocSets(vk.dev, &ai, &g_set) == VK_SUCCESS;
}

bool createWork(uint32_t n) {
    if (g_work) {
        vk.destroyView(vk.dev, g_workView, nullptr);
        vk.destroyImage(vk.dev, g_work, nullptr);
        vk.freeMem(vk.dev, g_workMem, nullptr);
        g_work = VK_NULL_HANDLE;
    }
    VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ci.imageType = VK_IMAGE_TYPE_2D;
    ci.format = VK_FORMAT_R8G8B8A8_UNORM;
    ci.extent = {n, n, 1};
    ci.mipLevels = ci.arrayLayers = 1;
    ci.samples = VK_SAMPLE_COUNT_1_BIT;
    ci.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    if (vk.createImage(vk.dev, &ci, nullptr, &g_work) != VK_SUCCESS) return false;
    VkMemoryRequirements mr;
    vk.memReq(vk.dev, g_work, &mr);
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.allocationSize = mr.size;
    ai.memoryTypeIndex = memType(mr.memoryTypeBits);
    if (ai.memoryTypeIndex == UINT32_MAX || vk.alloc(vk.dev, &ai, nullptr, &g_workMem) != VK_SUCCESS) return false;
    if (vk.bind(vk.dev, g_work, g_workMem, 0) != VK_SUCCESS) return false;
    VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vi.image = g_work;
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vi.format = ci.format;
    vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    if (vk.createView(vk.dev, &vi, nullptr, &g_workView) != VK_SUCCESS) return false;
    VkDescriptorImageInfo ii{VK_NULL_HANDLE, g_workView, VK_IMAGE_LAYOUT_GENERAL};
    VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, g_set, 0, 0, 1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, &ii};
    vk.updateSets(vk.dev, 1, &w, 0, nullptr);
    return true;
}

bool setup(const eyes::ScopeFrame& f) {
    if (g_failed) return false;
    if (g_sc.handle && g_size == f.width && g_srcFormat == f.format) return true;
    const bool rgba = f.format == VK_FORMAT_R8G8B8A8_UNORM || f.format == VK_FORMAT_R8G8B8A8_SRGB;
    const bool bgra = f.format == VK_FORMAT_B8G8R8A8_UNORM || f.format == VK_FORMAT_B8G8R8A8_SRGB;
    if ((!rgba && !bgra) || f.width != f.height) {
        VRLOG("scope: unsupported scope image (format %d, %ux%u) - no layer", int(f.format), f.width, f.height);
        g_failed = true;
        return false;
    }
    {
        vkq::Guard lock;
        vk.waitIdle(vk.dev);  // the old swapchain and work image may still be read
        if (g_sc.handle) xrDestroySwapchain(g_sc.handle);
        g_sc = {};
        if ((!g_pipe && !createPipeline()) || !createWork(f.width)) {
            VRLOG("scope: pipeline/work image creation failed");
            g_failed = true;
            return false;
        }
    }
    auto& rt = xr::runtime();
    const int64_t prefer[] = {VK_FORMAT_R8G8B8A8_SRGB};
    if (rt.pickColorFormat(prefer, 1) != VK_FORMAT_R8G8B8A8_SRGB ||
        !rt.createSwapchain(g_sc, VK_FORMAT_R8G8B8A8_SRGB, f.width, f.height, 1,
                            XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT)) {
        VRLOG("scope: no R8G8B8A8_SRGB swapchain - no layer");
        g_failed = true;
        return false;
    }
    g_size = f.width;
    g_srcFormat = f.format;
    VRLOG("scope: layer swapchain %ux%u (engine format %d)", f.width, f.height, int(f.format));
    return true;
}

void barrier(VkCommandBuffer cb, VkImage img, VkImageLayout from, VkImageLayout to, VkAccessFlags src, VkAccessFlags dst,
             VkPipelineStageFlags ss, VkPipelineStageFlags ds) {
    VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    b.srcAccessMask = src;
    b.dstAccessMask = dst;
    b.oldLayout = from;
    b.newLayout = to;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = img;
    b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vk.barrier(cb, ss, ds, 0, 0, nullptr, 0, nullptr, 1, &b);
}
}  // namespace

void onDevice(VkInstance inst, PFN_vkGetInstanceProcAddr gipa, VkPhysicalDevice pd, VkDevice dev, PFN_vkGetDeviceProcAddr gdpa) {
    vk.dev = dev;
    auto gmp = reinterpret_cast<PFN_vkGetPhysicalDeviceMemoryProperties>(gipa(inst, "vkGetPhysicalDeviceMemoryProperties"));
    if (gmp) gmp(pd, &vk.mem);
#define F(m, n) vk.m = reinterpret_cast<PFN_##n>(gdpa(dev, #n))
    F(createImage, vkCreateImage);
    F(destroyImage, vkDestroyImage);
    F(createView, vkCreateImageView);
    F(destroyView, vkDestroyImageView);
    F(memReq, vkGetImageMemoryRequirements);
    F(alloc, vkAllocateMemory);
    F(freeMem, vkFreeMemory);
    F(bind, vkBindImageMemory);
    F(createDsl, vkCreateDescriptorSetLayout);
    F(createPl, vkCreatePipelineLayout);
    F(createShader, vkCreateShaderModule);
    F(destroyShader, vkDestroyShaderModule);
    F(createCompute, vkCreateComputePipelines);
    F(createPool, vkCreateDescriptorPool);
    F(allocSets, vkAllocateDescriptorSets);
    F(updateSets, vkUpdateDescriptorSets);
    F(barrier, vkCmdPipelineBarrier);
    F(bindPipeline, vkCmdBindPipeline);
    F(bindSets, vkCmdBindDescriptorSets);
    F(push, vkCmdPushConstants);
    F(dispatch, vkCmdDispatch);
    F(copy, vkCmdCopyImage);
    F(waitIdle, vkDeviceWaitIdle);
#undef F
}

bool begin(const eyes::ScopeFrame& f) {
    g_acquired = false;
    if (!f.ready || !f.image || !setup(f)) return false;
    g_acquired = xr::runtime().acquire(g_sc, g_index);
    return g_acquired;
}

void record(VkCommandBuffer cb, const eyes::ScopeFrame& f, bool reticle) {
    if (!g_acquired) return;
    const uint32_t n = g_size;
    VkImageCopy c{};
    c.srcSubresource = c.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    c.extent = {n, n, 1};
    // engine image (TRANSFER_SRC) -> work image, raw bytes
    barrier(cb, g_work, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT,
            VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
    vk.copy(cb, f.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, g_work, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &c);
    barrier(cb, g_work, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL, VK_ACCESS_TRANSFER_WRITE_BIT,
            VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
    // mask + reticle in place
    const float pc[4] = {float(n), reticle ? 1.0f : 0.0f,
                         (g_srcFormat == VK_FORMAT_B8G8R8A8_UNORM || g_srcFormat == VK_FORMAT_B8G8R8A8_SRGB) ? 1.0f : 0.0f, 0};
    vk.bindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, g_pipe);
    vk.bindSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, g_pl, 0, 1, &g_set, 0, nullptr);
    vk.push(cb, g_pl, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof pc, pc);
    vk.dispatch(cb, (n + 7) / 8, (n + 7) / 8, 1);
    // work image -> layer swapchain (_SRGB twin: raw copy keeps the sRGB-encoded bytes)
    VkImage dst = g_sc.images[g_index];
    barrier(cb, g_work, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT,
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
    barrier(cb, dst, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT,
            VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
    vk.copy(cb, g_work, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &c);
    barrier(cb, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT,
            VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);
    static bool announced;
    if (!announced) VRLOG("scope: first scope layer image recorded");
    announced = true;
}

void end() {
    if (g_acquired) xr::runtime().release(g_sc);
    g_acquired = false;
}

XrSwapchain swapchain() { return g_sc.handle; }
uint32_t size() { return g_size; }
}  // namespace scopelayer
