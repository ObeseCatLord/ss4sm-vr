#include "vk/ammo_layer.hpp"
#include "core/ammo_counter.hpp"
#include "se4/ammo_counter.hpp"
#include "se4/log.hpp"
#include "xr/input.hpp"
#include "xr/runtime.hpp"
#include <windows.h>

namespace ammolayer {
namespace {
constexpr uint32_t kW = 256, kHalf = 128, kH = 2 * kHalf;  // one readout per half (2:1, as the quad)
constexpr int kSlots = 3;                                  // the mirror's frames in flight

struct Fn {
    VkDevice dev = VK_NULL_HANDLE;
    VkPhysicalDeviceMemoryProperties mem{};
    PFN_vkCreateBuffer createBuffer;
    PFN_vkGetBufferMemoryRequirements bufReq;
    PFN_vkAllocateMemory alloc;
    PFN_vkBindBufferMemory bind;
    PFN_vkMapMemory map;
    PFN_vkCmdPipelineBarrier barrier;
    PFN_vkCmdCopyBufferToImage copy;
} vk;

xr::Swapchain g_sc;
bool g_failed, g_acquired;
bool g_filled;  // this frame's image was recorded (layers() runs after end() released it)
uint32_t g_index;
VkBuffer g_staging = VK_NULL_HANDLE;
uint8_t* g_stagingPtr;
ammo::Hand g_hands[2];
int g_count;

bool setup() {
    if (g_sc.handle) return true;
    if (g_failed || !vk.dev) return false;
    auto& rt = xr::runtime();
    const int64_t prefer[] = {VK_FORMAT_R8G8B8A8_SRGB};
    if (rt.pickColorFormat(prefer, 1) != VK_FORMAT_R8G8B8A8_SRGB ||
        !rt.createSwapchain(g_sc, VK_FORMAT_R8G8B8A8_SRGB, kW, kH, 1, XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT)) {
        VRLOG("ammo: no R8G8B8A8_SRGB swapchain - no counters");
        g_failed = true;
        return false;
    }
    VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bi.size = VkDeviceSize(kW) * kH * 4 * kSlots;
    bi.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    VkMemoryRequirements mr;
    VkDeviceMemory mem;
    void* p = nullptr;
    uint32_t type = UINT32_MAX;
    if (vk.createBuffer(vk.dev, &bi, nullptr, &g_staging) == VK_SUCCESS) {
        vk.bufReq(vk.dev, g_staging, &mr);
        const VkMemoryPropertyFlags want = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
        for (uint32_t i = 0; i < vk.mem.memoryTypeCount && type == UINT32_MAX; ++i)
            if ((mr.memoryTypeBits & (1u << i)) && (vk.mem.memoryTypes[i].propertyFlags & want) == want) type = i;
    }
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.allocationSize = g_staging ? mr.size : 0;
    ai.memoryTypeIndex = type;
    if (!g_staging || type == UINT32_MAX || vk.alloc(vk.dev, &ai, nullptr, &mem) != VK_SUCCESS || vk.bind(vk.dev, g_staging, mem, 0) != VK_SUCCESS ||
        vk.map(vk.dev, mem, 0, VK_WHOLE_SIZE, 0, &p) != VK_SUCCESS) {
        VRLOG("ammo: staging buffer FAILED - no counters");
        g_failed = true;
        return false;
    }
    g_stagingPtr = static_cast<uint8_t*>(p);
    VRLOG("ammo: counter layer swapchain %ux%u", kW, kH);
    return true;
}

void barrier(VkCommandBuffer cb, VkImage img, VkImageLayout from, VkImageLayout to, VkAccessFlags src, VkAccessFlags dst, VkPipelineStageFlags ss,
             VkPipelineStageFlags ds) {
    VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    b.srcAccessMask = src, b.dstAccessMask = dst;
    b.oldLayout = from, b.newLayout = to;
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
    F(createBuffer, vkCreateBuffer);
    F(bufReq, vkGetBufferMemoryRequirements);
    F(alloc, vkAllocateMemory);
    F(bind, vkBindBufferMemory);
    F(map, vkMapMemory);
    F(barrier, vkCmdPipelineBarrier);
    F(copy, vkCmdCopyBufferToImage);
#undef F
}

int begin() {
    g_acquired = g_filled = false;
    g_count = 0;
    for (int h = 0; h < 2; ++h) {
        g_hands[h] = ammo::hand(h);
        g_count += g_hands[h].visible && xr::input().hand(h).active;
    }
    if (g_count && ammo::sceneDrawing()) return g_count = 0;  // drawn in the eyes' scenes (depth-tested) instead
    if (!g_count || !setup()) return g_count = 0;
    g_acquired = xr::runtime().acquire(g_sc, g_index);
    if (!g_acquired) g_count = 0;
    return g_count;
}

void record(VkCommandBuffer cb, int slot) {
    if (!g_acquired) return;
    slot = ((slot % kSlots) + kSlots) % kSlots;
    uint8_t* base = g_stagingPtr + size_t(kW) * kH * 4 * slot;
    const double t = double(GetTickCount64()) / 1000.0;
    for (int h = 0; h < 2; ++h) {
        uint8_t* half = base + size_t(kW) * kHalf * 4 * h;
        if (g_hands[h].visible) core::ammo::rasterize(half, kW, kHalf, g_hands[h].count, core::ammo::colour(g_hands[h].count, g_hands[h].max, t));
        else memset(half, 0, size_t(kW) * kHalf * 4);
    }
    VkImage dst = g_sc.images[g_index];
    barrier(cb, dst, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
            VK_PIPELINE_STAGE_TRANSFER_BIT);
    VkBufferImageCopy c{};
    c.bufferOffset = VkDeviceSize(kW) * kH * 4 * slot;
    c.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    c.imageExtent = {kW, kH, 1};
    vk.copy(cb, g_staging, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &c);
    barrier(cb, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT,
            VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);
    g_filled = true;
}

void end() {
    if (g_acquired) xr::runtime().release(g_sc);
    g_acquired = false;
}

int layers(XrSpace space, XrCompositionLayerQuad out[2]) {
    int n = 0;
    for (int h = 0; h < 2 && g_filled; ++h) {
        const ammo::Hand& a = g_hands[h];
        const auto& in = xr::input().hand(h);
        if (!a.visible || !in.active) continue;
        const XrPosef& g = in.grip;
        const core::Quat gq{g.orientation.x, g.orientation.y, g.orientation.z, g.orientation.w};
        const core::Quat q = core::normalize(core::multiply(gq, a.gripToSpot));
        const core::Vec3 p = core::Vec3{g.position.x, g.position.y, g.position.z} + core::rotate(gq, a.gripOffset);
        XrCompositionLayerQuad& l = out[n++];
        l = {XR_TYPE_COMPOSITION_LAYER_QUAD};
        l.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT | XR_COMPOSITION_LAYER_UNPREMULTIPLIED_ALPHA_BIT;
        l.space = space;
        l.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
        l.subImage.swapchain = g_sc.handle;
        l.subImage.imageRect = {{0, int32_t(kHalf * h)}, {int32_t(kW), int32_t(kHalf)}};
        l.pose.orientation = {q.x, q.y, q.z, q.w};
        l.pose.position = {p.x, p.y, p.z};
        l.size = {a.width, a.width * float(kHalf) / float(kW)};
    }
    return n;
}
}  // namespace ammolayer
