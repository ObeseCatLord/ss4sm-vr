// Vulkan interception for the game's own Vulkan backend (GfxVLK).
//
// - vkCreateInstance / vkCreateDevice issued from the game module are created through
//   XR_KHR_vulkan_enable2 so the runtime (wineopenxr under Proton) can inject what it needs; the
//   multiview feature is added to the device. Calls from other modules pass straight through.
// - Queue functions on the XR graphics queue are serialised with the runtime (vk/queue_lock).
// - Milestone "mirror": each presented game image is blitted into a world-locked quad layer.
#include "vk/vk_layer.hpp"
#include "se4/build.hpp"
#include "se4/log.hpp"
#include "vk/queue_lock.hpp"
#include "xr/runtime.hpp"
#include <windows.h>
#include <MinHook.h>
#include <intrin.h>
#include <cmath>
#include <cstring>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace vk_layer {
namespace {

PFN_vkGetInstanceProcAddr realGIPA;
PFN_vkCreateInstance realCreateInstance;
PFN_vkCreateDevice realCreateDevice;
PFN_vkGetDeviceProcAddr realGDPA;

// Device-level real functions (resolved through realGDPA for the game device).
PFN_vkQueueSubmit realQueueSubmit;
PFN_vkQueueSubmit2 realQueueSubmit2;
PFN_vkQueuePresentKHR realQueuePresent;
PFN_vkQueueWaitIdle realQueueWaitIdle;
PFN_vkDeviceWaitIdle realDeviceWaitIdle;
PFN_vkCreateSwapchainKHR realCreateSwapchain;
PFN_vkGetSwapchainImagesKHR realGetSwapchainImages;
PFN_vkGetDeviceQueue realGetDeviceQueue;

struct GameVk {
    VkInstance instance = VK_NULL_HANDLE;
    VkPhysicalDevice physical = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    uint32_t graphicsFamily = UINT32_MAX;
    VkQueue xrQueue = VK_NULL_HANDLE;  // family graphicsFamily, index 0
    bool xrInstance = false, xrDevice = false;
} g;

struct SwapchainInfo {
    VkFormat format;
    VkExtent2D extent;
    std::vector<VkImage> images;
};
std::mutex g_swapLock;
std::unordered_map<VkSwapchainKHR, SwapchainInfo> g_swapchains;

// Mirror quad state (render resources live on the game device).
struct Mirror {
    bool ready = false, failed = false;
    xr::Swapchain sc;
    VkCommandPool pool = VK_NULL_HANDLE;
    static constexpr int kRing = 3;
    VkCommandBuffer cmd[kRing]{};
    VkFence fence[kRing]{};
    VkSemaphore done[kRing]{};
    int slot = 0;
    XrPosef pose{};
    bool placed = false;
} m;

#define DFN(name) reinterpret_cast<PFN_##name>(realGDPA(g.device, #name))

bool callerInGame(void* ret) {
    const auto& b = se4::build();
    if (!b.table) return false;
    auto a = reinterpret_cast<uintptr_t>(ret);
    return a >= b.base && a < b.base + b.table->sizeOfImage;
}

// Re-entrancy guard: the runtime calls vkCreateInstance/vkCreateDevice itself while we are inside
// xrCreateVulkan*KHR; those inner calls must reach the real functions.
DWORD g_redirectThread;

VKAPI_ATTR VkResult VKAPI_CALL hookCreateInstance(const VkInstanceCreateInfo* ci, const VkAllocationCallbacks* a, VkInstance* out) {
    void* ret = __builtin_return_address(0);
    if (g_redirectThread == GetCurrentThreadId() || g.instance || !callerInGame(ret)) return realCreateInstance(ci, a, out);
    VRLOG("vk: game vkCreateInstance (%u extensions)", ci->enabledExtensionCount);
    auto& rt = xr::runtime();
    if (!rt.active() && !rt.initialize()) {
        VRLOG("vk: no OpenXR system - flat passthrough");
        VkResult r = realCreateInstance(ci, a, out);
        if (r == VK_SUCCESS) g.instance = *out;
        return r;
    }
    g_redirectThread = GetCurrentThreadId();
    VkResult r = rt.createVulkanInstance(realGIPA, ci, out);
    g_redirectThread = 0;
    VRLOG("vk: xrCreateVulkanInstanceKHR -> %d", int(r));
    if (r == VK_SUCCESS) {
        g.instance = *out;
        g.xrInstance = true;
    }
    return r;
}

VKAPI_ATTR VkResult VKAPI_CALL hookCreateDevice(VkPhysicalDevice pd, const VkDeviceCreateInfo* ci, const VkAllocationCallbacks* a, VkDevice* out) {
    if (g_redirectThread == GetCurrentThreadId() || g.device || !g.xrInstance) {
        VkResult r = realCreateDevice(pd, ci, a, out);
        if (r == VK_SUCCESS && !g.device && g.instance) g.device = *out, g.physical = pd;
        return r;
    }
    auto& rt = xr::runtime();
    VkPhysicalDevice want = rt.requiredPhysicalDevice(g.instance);
    if (want != pd) VRLOG("vk: WARNING game picked a different physical device than the XR runtime requires");

    // Add the multiview feature unless the game already chains one.
    VkDeviceCreateInfo copy = *ci;
    VkPhysicalDeviceMultiviewFeatures mv{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MULTIVIEW_FEATURES};
    mv.multiview = VK_TRUE;
    bool have = false;
    for (auto* s = static_cast<const VkBaseInStructure*>(ci->pNext); s; s = s->pNext)
        if (s->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MULTIVIEW_FEATURES || s->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES)
            have = true;
    if (!have) {
        mv.pNext = const_cast<void*>(ci->pNext);
        copy.pNext = &mv;
    }
    uint32_t family = UINT32_MAX;
    for (uint32_t i = 0; i < ci->queueCreateInfoCount; ++i) {
        uint32_t f = ci->pQueueCreateInfos[i].queueFamilyIndex;
        if (family == UINT32_MAX) family = f;  // engine log: family 0 is its graphics queue
    }
    g_redirectThread = GetCurrentThreadId();
    VkResult r = rt.createVulkanDevice(realGIPA, pd, &copy, out);
    g_redirectThread = 0;
    VRLOG("vk: xrCreateVulkanDeviceKHR -> %d (multiview %s, graphics family %u)", int(r), have ? "game" : "added", family);
    if (r != VK_SUCCESS) return r;
    g.device = *out;
    g.physical = pd;
    g.graphicsFamily = family;
    g.xrDevice = true;

    realQueueSubmit = DFN(vkQueueSubmit);
    realQueueSubmit2 = DFN(vkQueueSubmit2);
    if (!realQueueSubmit2) realQueueSubmit2 = reinterpret_cast<PFN_vkQueueSubmit2>(realGDPA(g.device, "vkQueueSubmit2KHR"));
    realQueuePresent = DFN(vkQueuePresentKHR);
    realQueueWaitIdle = DFN(vkQueueWaitIdle);
    realDeviceWaitIdle = DFN(vkDeviceWaitIdle);
    realCreateSwapchain = DFN(vkCreateSwapchainKHR);
    realGetSwapchainImages = DFN(vkGetSwapchainImagesKHR);
    realGetDeviceQueue = DFN(vkGetDeviceQueue);
    realGetDeviceQueue(g.device, family, 0, &g.xrQueue);
    if (!rt.createSession(g.instance, g.physical, g.device, family, 0)) VRLOG("vk: XR session unavailable - flat");
    return r;
}

// ---- queue serialisation ----
VKAPI_ATTR VkResult VKAPI_CALL hookQueueSubmit(VkQueue q, uint32_t n, const VkSubmitInfo* s, VkFence f) {
    if (q != g.xrQueue) return realQueueSubmit(q, n, s, f);
    vkq::Guard lock;
    return realQueueSubmit(q, n, s, f);
}
VKAPI_ATTR VkResult VKAPI_CALL hookQueueSubmit2(VkQueue q, uint32_t n, const VkSubmitInfo2* s, VkFence f) {
    if (q != g.xrQueue) return realQueueSubmit2(q, n, s, f);
    vkq::Guard lock;
    return realQueueSubmit2(q, n, s, f);
}
VKAPI_ATTR VkResult VKAPI_CALL hookQueueWaitIdle(VkQueue q) {
    if (q != g.xrQueue) return realQueueWaitIdle(q);
    vkq::Guard lock;
    return realQueueWaitIdle(q);
}
VKAPI_ATTR VkResult VKAPI_CALL hookDeviceWaitIdle(VkDevice d) {
    vkq::Guard lock;
    return realDeviceWaitIdle(d);
}

VKAPI_ATTR VkResult VKAPI_CALL hookCreateSwapchain(VkDevice d, const VkSwapchainCreateInfoKHR* ci, const VkAllocationCallbacks* a, VkSwapchainKHR* out) {
    VkSwapchainCreateInfoKHR c = *ci;
    c.imageUsage |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;  // mirror blits from the presented image
    VkResult r = realCreateSwapchain(d, &c, a, out);
    if (r == VK_SUCCESS) {
        uint32_t n = 0;
        realGetSwapchainImages(d, *out, &n, nullptr);
        SwapchainInfo info{c.imageFormat, c.imageExtent, std::vector<VkImage>(n)};
        realGetSwapchainImages(d, *out, &n, info.images.data());
        std::lock_guard<std::mutex> l(g_swapLock);
        g_swapchains[*out] = std::move(info);
        VRLOG("vk: game swapchain %ux%u format %d, %u images", c.imageExtent.width, c.imageExtent.height, int(c.imageFormat), n);
    }
    return r;
}

// ---- mirror milestone ----
bool setupMirror(const SwapchainInfo& sw) {
    auto& rt = xr::runtime();
    const int64_t prefer[] = {VK_FORMAT_B8G8R8A8_SRGB, VK_FORMAT_R8G8B8A8_SRGB, VK_FORMAT_B8G8R8A8_UNORM, VK_FORMAT_R8G8B8A8_UNORM};
    int64_t fmt = rt.pickColorFormat(prefer, 4);
    if (!rt.createSwapchain(m.sc, fmt, sw.extent.width, sw.extent.height, 1,
                            XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT))
        return false;
    VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pci.queueFamilyIndex = g.graphicsFamily;
    if (DFN(vkCreateCommandPool)(g.device, &pci, nullptr, &m.pool) != VK_SUCCESS) return false;
    VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    ai.commandPool = m.pool;
    ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    ai.commandBufferCount = Mirror::kRing;
    DFN(vkAllocateCommandBuffers)(g.device, &ai, m.cmd);
    for (int i = 0; i < Mirror::kRing; ++i) {
        VkFenceCreateInfo fi{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        fi.flags = VK_FENCE_CREATE_SIGNALED_BIT;
        DFN(vkCreateFence)(g.device, &fi, nullptr, &m.fence[i]);
        VkSemaphoreCreateInfo si{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        DFN(vkCreateSemaphore)(g.device, &si, nullptr, &m.done[i]);
    }
    return true;
}

XrPosef placeInFront(const XrPosef& head, float dist) {
    // Yaw-only placement: forward is -Z in OpenXR.
    const XrQuaternionf& q = head.orientation;
    float fx = -2.0f * (q.x * q.z + q.w * q.y), fz = -(1.0f - 2.0f * (q.x * q.x + q.y * q.y));
    float len = std::sqrt(fx * fx + fz * fz);
    if (len < 1e-4f) fx = 0, fz = -1, len = 1;
    fx /= len, fz /= len;
    float yaw = std::atan2(-fx, -fz);
    XrPosef p{};
    p.orientation = {0, std::sin(yaw * 0.5f), 0, std::cos(yaw * 0.5f)};
    p.position = {head.position.x + fx * dist, head.position.y, head.position.z + fz * dist};
    return p;
}

void imageBarrier(VkCommandBuffer cb, VkImage img, VkImageLayout from, VkImageLayout to, VkAccessFlags src, VkAccessFlags dst,
                  VkPipelineStageFlags ss, VkPipelineStageFlags ds) {
    VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    b.oldLayout = from;
    b.newLayout = to;
    b.srcAccessMask = src;
    b.dstAccessMask = dst;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = img;
    b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    DFN(vkCmdPipelineBarrier)(cb, ss, ds, 0, 0, nullptr, 0, nullptr, 1, &b);
}

VKAPI_ATTR VkResult VKAPI_CALL hookQueuePresent(VkQueue q, const VkPresentInfoKHR* pi) {
    auto& rt = xr::runtime();
    if (!g.xrDevice || pi->swapchainCount != 1) {
        if (q != g.xrQueue) return realQueuePresent(q, pi);
        vkq::Guard lock;
        return realQueuePresent(q, pi);
    }
    rt.pollEvents();
    SwapchainInfo sw;
    {
        std::lock_guard<std::mutex> l(g_swapLock);
        auto it = g_swapchains.find(pi->pSwapchains[0]);
        if (it != g_swapchains.end()) sw = it->second;
    }
    if (!rt.running() || sw.images.empty() || m.failed || q != g.xrQueue) {
        vkq::Guard lock;
        return realQueuePresent(q, pi);
    }
    if (!m.ready) {
        m.ready = setupMirror(sw);
        if (!m.ready) {
            m.failed = true;
            VRLOG("vk: mirror setup failed");
            vkq::Guard lock;
            return realQueuePresent(q, pi);
        }
    }

    XrFrameState fs;
    if (!rt.waitBeginFrame(fs)) {
        vkq::Guard lock;
        return realQueuePresent(q, pi);
    }
    if (!m.placed) {
        XrPosef head;
        if (rt.locateHead(fs.predictedDisplayTime, head)) {
            m.pose = placeInFront(head, 2.5f);
            m.placed = true;
        }
    }

    VkImage src = sw.images[pi->pImageIndices[0]];
    uint32_t idx = 0;
    bool haveImage = rt.acquire(m.sc, idx);
    VkSemaphore waitOn = VK_NULL_HANDLE;
    VkResult result;
    {
        vkq::Guard lock;
        int s = m.slot;
        m.slot = (m.slot + 1) % Mirror::kRing;
        DFN(vkWaitForFences)(g.device, 1, &m.fence[s], VK_TRUE, UINT64_MAX);
        DFN(vkResetFences)(g.device, 1, &m.fence[s]);
        VkCommandBuffer cb = m.cmd[s];
        DFN(vkResetCommandBuffer)(cb, 0);
        VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        DFN(vkBeginCommandBuffer)(cb, &bi);
        if (haveImage) {
            VkImage dst = m.sc.images[idx];
            imageBarrier(cb, src, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, 0, VK_ACCESS_TRANSFER_READ_BIT,
                         VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
            imageBarrier(cb, dst, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT,
                         VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
            VkImageBlit blit{};
            blit.srcSubresource = blit.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            blit.srcOffsets[1] = {int32_t(sw.extent.width), int32_t(sw.extent.height), 1};
            blit.dstOffsets[1] = {int32_t(m.sc.width), int32_t(m.sc.height), 1};
            DFN(vkCmdBlitImage)(cb, src, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, VK_FILTER_LINEAR);
            imageBarrier(cb, src, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_ACCESS_TRANSFER_READ_BIT, 0,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);
            imageBarrier(cb, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT,
                         VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);
        }
        DFN(vkEndCommandBuffer)(cb);
        std::vector<VkPipelineStageFlags> stages(pi->waitSemaphoreCount, VK_PIPELINE_STAGE_TRANSFER_BIT);
        VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        si.waitSemaphoreCount = pi->waitSemaphoreCount;
        si.pWaitSemaphores = pi->pWaitSemaphores;
        si.pWaitDstStageMask = stages.data();
        si.commandBufferCount = 1;
        si.pCommandBuffers = &cb;
        si.signalSemaphoreCount = 1;
        si.pSignalSemaphores = &m.done[s];
        realQueueSubmit(q, 1, &si, m.fence[s]);
        waitOn = m.done[s];
        // The runtime reads the image after release; finish the blit first.
        DFN(vkWaitForFences)(g.device, 1, &m.fence[s], VK_TRUE, UINT64_MAX);
        VkPresentInfoKHR p = *pi;
        p.waitSemaphoreCount = 1;
        p.pWaitSemaphores = &waitOn;
        result = realQueuePresent(q, &p);
    }
    if (haveImage) rt.release(m.sc);

    XrCompositionLayerQuad quad{XR_TYPE_COMPOSITION_LAYER_QUAD};
    quad.space = rt.localSpace();
    quad.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
    quad.subImage.swapchain = m.sc.handle;
    quad.subImage.imageRect = {{0, 0}, {int32_t(m.sc.width), int32_t(m.sc.height)}};
    quad.pose = m.pose;
    float w = 3.2f;
    quad.size = {w, w * float(m.sc.height) / float(m.sc.width)};
    const XrCompositionLayerBaseHeader* layers[] = {reinterpret_cast<XrCompositionLayerBaseHeader*>(&quad)};
    rt.endFrame(fs, layers, (haveImage && m.placed) ? 1 : 0);
    return result;
}

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL hookGDPA(VkDevice d, const char* name);

PFN_vkVoidFunction deviceOverride(const char* name) {
    if (!strcmp(name, "vkQueueSubmit")) return reinterpret_cast<PFN_vkVoidFunction>(hookQueueSubmit);
    if (!strcmp(name, "vkQueueSubmit2") || !strcmp(name, "vkQueueSubmit2KHR")) return reinterpret_cast<PFN_vkVoidFunction>(hookQueueSubmit2);
    if (!strcmp(name, "vkQueuePresentKHR")) return reinterpret_cast<PFN_vkVoidFunction>(hookQueuePresent);
    if (!strcmp(name, "vkQueueWaitIdle")) return reinterpret_cast<PFN_vkVoidFunction>(hookQueueWaitIdle);
    if (!strcmp(name, "vkDeviceWaitIdle")) return reinterpret_cast<PFN_vkVoidFunction>(hookDeviceWaitIdle);
    if (!strcmp(name, "vkCreateSwapchainKHR")) return reinterpret_cast<PFN_vkVoidFunction>(hookCreateSwapchain);
    if (!strcmp(name, "vkGetDeviceProcAddr")) return reinterpret_cast<PFN_vkVoidFunction>(hookGDPA);
    return nullptr;
}

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL hookGDPA(VkDevice d, const char* name) {
    PFN_vkVoidFunction real = realGDPA(d, name);
    if (!real || !name || d != g.device || !g.xrDevice) return real;
    if (auto o = deviceOverride(name)) return o;
    return real;
}

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL hookGIPA(VkInstance inst, const char* name) {
    if (name) {
        if (!strcmp(name, "vkCreateInstance")) return reinterpret_cast<PFN_vkVoidFunction>(hookCreateInstance);
        if (!strcmp(name, "vkCreateDevice")) return reinterpret_cast<PFN_vkVoidFunction>(hookCreateDevice);
        if (!strcmp(name, "vkGetDeviceProcAddr")) return reinterpret_cast<PFN_vkVoidFunction>(hookGDPA);
        if (inst && inst == g.instance && g.xrDevice)
            if (auto o = deviceOverride(name)) return o;
    }
    return realGIPA(inst, name);
}

template <class T> bool hook(HMODULE mod, const char* name, void* detour, T* orig) {
    void* target = reinterpret_cast<void*>(GetProcAddress(mod, name));
    if (!target) {
        VRLOG("vk: export %s missing", name);
        return false;
    }
    if (MH_CreateHook(target, detour, reinterpret_cast<void**>(orig)) != MH_OK || MH_EnableHook(target) != MH_OK) {
        VRLOG("vk: hook %s failed", name);
        return false;
    }
    return true;
}
}  // namespace

void onVulkanLoaded(void* module) {
    vkq::initLock();
    auto mod = static_cast<HMODULE>(module);
    bool ok = hook(mod, "vkGetInstanceProcAddr", reinterpret_cast<void*>(hookGIPA), &realGIPA) &&
              hook(mod, "vkCreateInstance", reinterpret_cast<void*>(hookCreateInstance), &realCreateInstance) &&
              hook(mod, "vkCreateDevice", reinterpret_cast<void*>(hookCreateDevice), &realCreateDevice) &&
              hook(mod, "vkGetDeviceProcAddr", reinterpret_cast<void*>(hookGDPA), &realGDPA);
    VRLOG("vk: hooks %s", ok ? "installed" : "incomplete");
}

}  // namespace vk_layer
