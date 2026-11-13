// Vulkan interception for the game's own Vulkan backend (GfxVLK).
//
// - vkCreateInstance / vkCreateDevice issued from the game module are created through
//   XR_KHR_vulkan_enable2 so the runtime (wineopenxr under Proton) can inject what it needs; the
//   multiview feature is added to the device. Calls from other modules pass straight through.
// - Queue functions on the XR graphics queue are serialised with the runtime (vk/queue_lock).
// - Milestone "mirror": each presented game image is blitted into a world-locked quad layer.
#include "vk/vk_layer.hpp"
#include "se4/build.hpp"
#include "se4/cvars.hpp"
#include "se4/procaddr.hpp"
#include "se4/log.hpp"
#include "se4/options.hpp"
#include "vk/queue_lock.hpp"
#include "xr/runtime.hpp"
#include "xr/input.hpp"
#include "se4/gamepad.hpp"
#include "se4/focus.hpp"
#include "se4/vr_device.hpp"
#include "vk/eye_images.hpp"
#include "vk/foveation.hpp"
#include "up/integrate.hpp"
#include "vk/ammo_layer.hpp"
#include "vk/scope_layer.hpp"
#include "vk/gpu_timing.hpp"
#include "se4/scope.hpp"
#include "se4/gfx_menu.hpp"
#include "up/upscaler.hpp"
#include "core/math.hpp"
#include <windows.h>
#include <MinHook.h>
#include <intrin.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace vk_layer {
void onGameVulkan();
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
PFN_vkCreateImage realCreateImage;
PFN_vkBeginCommandBuffer realBeginCB;
PFN_vkCmdPipelineBarrier realBarrier;
PFN_vkCmdPipelineBarrier2 realBarrier2;
PFN_vkDestroyImage realDestroyImage;

struct GameVk {
    VkInstance instance = VK_NULL_HANDLE;
    VkPhysicalDevice physical = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    uint32_t graphicsFamily = UINT32_MAX;
    VkQueue xrQueue = VK_NULL_HANDLE;  // family graphicsFamily, index 0
    bool xrInstance = false, xrDevice = false;
    bool flat = false;  // desktop play with an upscaler: the game device with the upscaler's extensions, no XR
} g;
bool active() { return g.xrDevice || g.flat; }  // the device-level hooks apply

HWND g_gameWnd;
PFN_vkCreateWin32SurfaceKHR realCreateWin32Surface;
VKAPI_ATTR VkResult VKAPI_CALL hookCreateWin32Surface(VkInstance i, const VkWin32SurfaceCreateInfoKHR* ci, const VkAllocationCallbacks* a,
                                                       VkSurfaceKHR* out) {
    if (!g_gameWnd) g_gameWnd = ci->hwnd;
    return realCreateWin32Surface(i, ci, a, out);
}

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


// Re-entrancy guard: the runtime calls vkCreateInstance/vkCreateDevice itself while we are inside
// xrCreateVulkan*KHR; those inner calls must reach the real functions.
DWORD g_redirectThread;

// Appends `extra` extension names to a create info's list (no duplicates, only those `available`).
std::vector<const char*> mergeExtensions(const char* const* have, uint32_t n, const char** extra, uint32_t m,
                                         const std::vector<VkExtensionProperties>& available) {
    std::vector<const char*> out(have, have + n);
    for (uint32_t i = 0; i < m; ++i) {
        bool dup = false, ok = false;
        for (auto* h : out) dup |= !strcmp(h, extra[i]);
        for (auto& e : available) ok |= !strcmp(e.extensionName, extra[i]);
        if (!dup && ok) out.push_back(extra[i]);
    }
    return out;
}

VKAPI_ATTR VkResult VKAPI_CALL hookCreateInstance(const VkInstanceCreateInfo* ci, const VkAllocationCallbacks* a, VkInstance* out) {
    void* ret = __builtin_return_address(0);
    if (!realCreateInstance) realCreateInstance = reinterpret_cast<PFN_vkCreateInstance>(realGIPA(nullptr, "vkCreateInstance"));
    if (g_redirectThread == GetCurrentThreadId() || g.instance || !se4::callerInGame(ret)) return realCreateInstance(ci, a, out);
    onGameVulkan();
    VRLOG("vk: game vkCreateInstance (%u extensions)", ci->enabledExtensionCount);
    auto& rt = xr::runtime();
    if (!se4::g_vrEnabled || !rt.ensure()) {
        // Desktop play: only the upscaler, when one is selected (or in desktop menu mode), needs its instance extensions.
        VkInstanceCreateInfo fci = *ci;
        const char* upExt[16];
        uint32_t upN = 0;
        std::vector<const char*> exts;
        if (upint::flatWanted()) up::instanceExtensions(upExt, upN, 16);
        if (upN) {
            auto enumerate = reinterpret_cast<PFN_vkEnumerateInstanceExtensionProperties>(realGIPA(nullptr, "vkEnumerateInstanceExtensionProperties"));
            uint32_t c = 0;
            enumerate(nullptr, &c, nullptr);
            std::vector<VkExtensionProperties> av(c);
            enumerate(nullptr, &c, av.data());
            exts = mergeExtensions(ci->ppEnabledExtensionNames, ci->enabledExtensionCount, upExt, upN, av);
            fci.enabledExtensionCount = uint32_t(exts.size());
            fci.ppEnabledExtensionNames = exts.data();
        }
        VRLOG(se4::g_vrEnabled ? "vk: no OpenXR system - flat passthrough" : "vk: desktop play (%u -> %u instance extensions)",
              ci->enabledExtensionCount, fci.enabledExtensionCount);
        VkResult r = realCreateInstance(&fci, a, out);
        if (r == VK_SUCCESS) g.instance = *out;
        return r;
    }
    // Upscaler (DLSS) instance extensions, when enabled.
    VkInstanceCreateInfo ici = *ci;
    const char* upExt[16];
    uint32_t upN = 0;
    up::instanceExtensions(upExt, upN, 16);
    std::vector<const char*> instExts;
    if (upN) {
        auto enumerate = reinterpret_cast<PFN_vkEnumerateInstanceExtensionProperties>(realGIPA(nullptr, "vkEnumerateInstanceExtensionProperties"));
        uint32_t c = 0;
        enumerate(nullptr, &c, nullptr);
        std::vector<VkExtensionProperties> av(c);
        enumerate(nullptr, &c, av.data());
        instExts = mergeExtensions(ci->ppEnabledExtensionNames, ci->enabledExtensionCount, upExt, upN, av);
        ici.enabledExtensionCount = uint32_t(instExts.size());
        ici.ppEnabledExtensionNames = instExts.data();
        VRLOG("vk: instance extensions %u -> %u for the upscaler", ci->enabledExtensionCount, ici.enabledExtensionCount);
    }
    g_redirectThread = GetCurrentThreadId();
    VkResult r = rt.createVulkanInstance(realGIPA, &ici, out);
    g_redirectThread = 0;
    VRLOG("vk: xrCreateVulkanInstanceKHR -> %d", int(r));
    if (r == VK_SUCCESS) {
        g.instance = *out;
        g.xrInstance = true;
    }
    return r;
}

// Device-level entry points and the modules that record into the game's command buffers (both modes).
void setupDevice(VkPhysicalDevice pd, uint32_t family) {
    realQueueSubmit = DFN(vkQueueSubmit);
    realQueueSubmit2 = DFN(vkQueueSubmit2);
    if (!realQueueSubmit2) realQueueSubmit2 = reinterpret_cast<PFN_vkQueueSubmit2>(realGDPA(g.device, "vkQueueSubmit2KHR"));
    realQueuePresent = DFN(vkQueuePresentKHR);
    realQueueWaitIdle = DFN(vkQueueWaitIdle);
    realDeviceWaitIdle = DFN(vkDeviceWaitIdle);
    realCreateSwapchain = DFN(vkCreateSwapchainKHR);
    realGetSwapchainImages = DFN(vkGetSwapchainImagesKHR);
    realGetDeviceQueue = DFN(vkGetDeviceQueue);
    realCreateImage = DFN(vkCreateImage);
    realBeginCB = DFN(vkBeginCommandBuffer);
    realBarrier = DFN(vkCmdPipelineBarrier);
    realBarrier2 = DFN(vkCmdPipelineBarrier2);
    if (!realBarrier2) realBarrier2 = reinterpret_cast<PFN_vkCmdPipelineBarrier2>(realGDPA(g.device, "vkCmdPipelineBarrier2KHR"));
    realDestroyImage = DFN(vkDestroyImage);
    realGetDeviceQueue(g.device, family, 0, &g.xrQueue);
    if (!g.flat) fov::onDevice(g.instance, realGIPA, realGDPA, pd, g.device, g.xrQueue, family);
    gputime::onDevice(g.instance, realGIPA, pd, g.device, realGDPA, family);
    wchar_t dir[MAX_PATH];
    se4::payloadDirectory(dir, MAX_PATH);
    up::init(g.instance, pd, g.device, realGIPA, realGDPA, g.xrQueue, family, dir);
    upint::onDevice(g.instance, realGIPA, pd, g.device, realGDPA);
    gfxmenu::onUpscalerReady();  // menu mode (desktop or VR): DLSS / FSR 3.1 in the Rescaling Filter list, the sharpening slider
    if (!g.flat) scopelayer::onDevice(g.instance, realGIPA, pd, g.device, realGDPA);
    if (!g.flat) ammolayer::onDevice(g.instance, realGIPA, pd, g.device, realGDPA);
}

// Desktop play with an upscaler: the game's own device plus the upscaler's device extensions; no XR, no foveation.
VkResult createFlatDevice(VkPhysicalDevice pd, const VkDeviceCreateInfo* ci, const VkAllocationCallbacks* a, VkDevice* out) {
    VkDeviceCreateInfo copy = *ci;
    const char* upDev[24];
    uint32_t upDevN = 0;
    up::deviceExtensions(g.instance, pd, upDev, upDevN, 24);
    std::vector<const char*> devExts;
    if (upDevN) {
        auto enumerate = reinterpret_cast<PFN_vkEnumerateDeviceExtensionProperties>(realGIPA(g.instance, "vkEnumerateDeviceExtensionProperties"));
        uint32_t c = 0;
        enumerate(pd, nullptr, &c, nullptr);
        std::vector<VkExtensionProperties> av(c);
        enumerate(pd, nullptr, &c, av.data());
        devExts = mergeExtensions(copy.ppEnabledExtensionNames, copy.enabledExtensionCount, upDev, upDevN, av);
        copy.enabledExtensionCount = uint32_t(devExts.size());
        copy.ppEnabledExtensionNames = devExts.data();
    }
    const uint32_t family = ci->queueCreateInfoCount ? ci->pQueueCreateInfos[0].queueFamilyIndex : 0;
    VkResult r = realCreateDevice(pd, &copy, a, out);
    VRLOG("vk: desktop device -> %d (%u -> %u extensions, graphics family %u)", int(r), ci->enabledExtensionCount,
          copy.enabledExtensionCount, family);
    if (r != VK_SUCCESS) return r;
    g.device = *out;
    g.physical = pd;
    g.graphicsFamily = family;
    g.flat = true;
    setupDevice(pd, family);
    return r;
}

VKAPI_ATTR VkResult VKAPI_CALL hookCreateDevice(VkPhysicalDevice pd, const VkDeviceCreateInfo* ci, const VkAllocationCallbacks* a, VkDevice* out) {
    if (!realCreateDevice) realCreateDevice = reinterpret_cast<PFN_vkCreateDevice>(realGIPA(g.instance, "vkCreateDevice"));
    if (!realGDPA) realGDPA = reinterpret_cast<PFN_vkGetDeviceProcAddr>(realGIPA(g.instance, "vkGetDeviceProcAddr"));
    if (!g.xrInstance && !g.device && g.instance && g_redirectThread != GetCurrentThreadId() && upint::flatWanted())
        return createFlatDevice(pd, ci, a, out);
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
    // Foveated rendering: decide from the headset, enable attachment shading rates on the game's chain.
    fov::decide();
    alignas(16) static unsigned char fovStorage[64];
    fov::patchDeviceCreate(g.instance, realGIPA, pd, copy, fovStorage, sizeof fovStorage);
    // Upscaler (DLSS) device extensions, when enabled and supported.
    const char* upDev[24];
    uint32_t upDevN = 0;
    up::deviceExtensions(g.instance, pd, upDev, upDevN, 24);
    std::vector<const char*> devExts;
    if (upDevN) {
        auto enumerate = reinterpret_cast<PFN_vkEnumerateDeviceExtensionProperties>(realGIPA(g.instance, "vkEnumerateDeviceExtensionProperties"));
        uint32_t c = 0;
        enumerate(pd, nullptr, &c, nullptr);
        std::vector<VkExtensionProperties> av(c);
        enumerate(pd, nullptr, &c, av.data());
        devExts = mergeExtensions(copy.ppEnabledExtensionNames, copy.enabledExtensionCount, upDev, upDevN, av);
        VRLOG("vk: device extensions %u -> %u for the upscaler", copy.enabledExtensionCount, uint32_t(devExts.size()));
        copy.enabledExtensionCount = uint32_t(devExts.size());
        copy.ppEnabledExtensionNames = devExts.data();
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

    setupDevice(pd, family);
    if (!rt.createSession(g.instance, g.physical, g.device, family, 0)) VRLOG("vk: XR session unavailable - flat");
    else se4::applyVrFramePacing();
    return r;
}

// ---- queue serialisation ----
VKAPI_ATTR VkResult VKAPI_CALL hookQueueSubmit(VkQueue q, uint32_t n, const VkSubmitInfo* s, VkFence f) {
    if (q != g.xrQueue) return realQueueSubmit(q, n, s, f);
    vkq::Guard lock;
    return gputime::submit(realQueueSubmit, q, n, s, f);
}
VKAPI_ATTR VkResult VKAPI_CALL hookQueueSubmit2(VkQueue q, uint32_t n, const VkSubmitInfo2* s, VkFence f) {
    if (q != g.xrQueue) return realQueueSubmit2(q, n, s, f);
    vkq::Guard lock;
    return gputime::submit2(realQueueSubmit2, q, n, s, f);
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

// ---- engine image tracking (eye render targets) ----
VKAPI_ATTR VkResult VKAPI_CALL hookCreateImage(VkDevice d, const VkImageCreateInfo* ci, const VkAllocationCallbacks* a, VkImage* out) {
    VkImageCreateInfo c = *ci;
    upint::patchImage(c);
    ci = &c;
    VkResult r = realCreateImage(d, ci, a, out);
    if (r == VK_SUCCESS) {
        eyes::onImageCreated(*out, *ci);
        upint::onImageCreated(*out, *ci);
    }
    return r;
}
VKAPI_ATTR VkResult VKAPI_CALL hookBeginCB(VkCommandBuffer cb, const VkCommandBufferBeginInfo* bi) {
    VkResult r = realBeginCB(cb, bi);
    if (r == VK_SUCCESS) upint::onBeginCommandBuffer(cb);
    return r;
}
VKAPI_ATTR void VKAPI_CALL hookBarrier(VkCommandBuffer cb, VkPipelineStageFlags ss, VkPipelineStageFlags ds, VkDependencyFlags df,
                                       uint32_t nm, const VkMemoryBarrier* m, uint32_t nb, const VkBufferMemoryBarrier* b, uint32_t ni,
                                       const VkImageMemoryBarrier* im) {
    for (uint32_t i = 0; i < ni; ++i) upint::onImageBarrier(cb, im[i].image, im[i].oldLayout, im[i].newLayout);
    realBarrier(cb, ss, ds, df, nm, m, nb, b, ni, im);
}
VKAPI_ATTR void VKAPI_CALL hookBarrier2(VkCommandBuffer cb, const VkDependencyInfo* di) {
    for (uint32_t i = 0; i < di->imageMemoryBarrierCount; ++i)
        upint::onImageBarrier(cb, di->pImageMemoryBarriers[i].image, di->pImageMemoryBarriers[i].oldLayout,
                              di->pImageMemoryBarriers[i].newLayout);
    realBarrier2(cb, di);
}
PFN_vkCreateRenderPass nextCreateRenderPass;
PFN_vkCreateRenderPass2 nextCreateRenderPass2;
VKAPI_ATTR VkResult VKAPI_CALL hookCreateRenderPassObs(VkDevice d, const VkRenderPassCreateInfo* ci, const VkAllocationCallbacks* a,
                                                       VkRenderPass* out) {
    VkRenderPassCreateInfo c = *ci;
    std::vector<VkAttachmentDescription> att;
    const VkResult r = nextCreateRenderPass(d, upint::patchPass(c, att) ? &c : ci, a, out);
    if (r == VK_SUCCESS) upint::traceRenderPass(*out, ci->pAttachments, ci->attachmentCount);
    return r;
}
VKAPI_ATTR VkResult VKAPI_CALL hookCreateRenderPass2Obs(VkDevice d, const VkRenderPassCreateInfo2* ci, const VkAllocationCallbacks* a,
                                                        VkRenderPass* out) {
    VkRenderPassCreateInfo2 c = *ci;
    std::vector<VkAttachmentDescription2> att;
    const VkResult r = nextCreateRenderPass2(d, upint::patchPass2(c, att) ? &c : ci, a, out);
    if (r == VK_SUCCESS) upint::traceRenderPass2(*out, ci->pAttachments, ci->attachmentCount);
    return r;
}
// Lab pass trace (upscale_pass_trace): image views and framebuffers, to see which passes use the scene depth.
PFN_vkCmdSetViewport realSetViewport;
VKAPI_ATTR void VKAPI_CALL hookSetViewport(VkCommandBuffer cb, uint32_t first, uint32_t n, const VkViewport* v) {
    for (uint32_t i = 0; i < n; ++i)
        if (v[i].minDepth != 0.0f || v[i].maxDepth != 1.0f) {
            upint::onViewport(v[i].minDepth, v[i].maxDepth);
            if (!upint::passTraceOn()) continue;
            static int logged;
            static float lastMin = -1, lastMax = -1;
            if ((v[i].minDepth != lastMin || v[i].maxDepth != lastMax) && logged++ < 40)
                VRLOG("passtrace: viewport depth range %.6f..%.6f (%.0fx%.0f)", v[i].minDepth, v[i].maxDepth, v[i].width, v[i].height);
            lastMin = v[i].minDepth, lastMax = v[i].maxDepth;
        }
    realSetViewport(cb, first, n, v);
}
PFN_vkCreateImageView realCreateView;
PFN_vkCreateFramebuffer realCreateFb;
VKAPI_ATTR VkResult VKAPI_CALL hookCreateView(VkDevice d, const VkImageViewCreateInfo* ci, const VkAllocationCallbacks* a, VkImageView* out) {
    const VkResult r = realCreateView(d, ci, a, out);
    if (r == VK_SUCCESS) upint::traceView(*out, ci->image);
    return r;
}
VKAPI_ATTR VkResult VKAPI_CALL hookCreateFb(VkDevice d, const VkFramebufferCreateInfo* ci, const VkAllocationCallbacks* a, VkFramebuffer* out) {
    const VkResult r = realCreateFb(d, ci, a, out);
    if (r == VK_SUCCESS) upint::traceFramebuffer(*out, ci->renderPass, ci->pAttachments, (ci->flags & VK_FRAMEBUFFER_CREATE_IMAGELESS_BIT) ? 0 : ci->attachmentCount);
    return r;
}
PFN_vkCmdEndRenderPass realEndPass;
PFN_vkCmdEndRenderPass2 realEndPass2;
VKAPI_ATTR void VKAPI_CALL hookEndPass(VkCommandBuffer cb) {
    upint::onEndPass(cb);
    realEndPass(cb);
}
VKAPI_ATTR void VKAPI_CALL hookEndPass2(VkCommandBuffer cb, const VkSubpassEndInfo* ei) {
    upint::onEndPass(cb);
    realEndPass2(cb, ei);
}
PFN_vkCmdBeginRenderPass nextBeginPass;
PFN_vkCmdBeginRenderPass2 nextBeginPass2;
VKAPI_ATTR void VKAPI_CALL hookBeginPassObs(VkCommandBuffer cb, const VkRenderPassBeginInfo* bi, VkSubpassContents c) {
    upint::onBeginPass(cb);
    upint::traceBeginPass(cb, bi);
    nextBeginPass(cb, bi, c);
}
VKAPI_ATTR void VKAPI_CALL hookBeginPass2Obs(VkCommandBuffer cb, const VkRenderPassBeginInfo* bi, const VkSubpassBeginInfo* si) {
    upint::onBeginPass(cb);
    upint::traceBeginPass(cb, bi);
    nextBeginPass2(cb, bi, si);
}
VKAPI_ATTR void VKAPI_CALL hookDestroyImage(VkDevice d, VkImage img, const VkAllocationCallbacks* a) {
    eyes::onImageDestroyed(img);
    realDestroyImage(d, img, a);
}

// ---- mirror milestone ----
bool setupMirror(const SwapchainInfo& sw) {
    auto& rt = xr::runtime();
    int64_t twin = sw.format == VK_FORMAT_R8G8B8A8_UNORM ? VK_FORMAT_R8G8B8A8_SRGB : VK_FORMAT_B8G8R8A8_SRGB;
    const int64_t prefer[] = {twin, VK_FORMAT_B8G8R8A8_SRGB, VK_FORMAT_R8G8B8A8_SRGB};
    int64_t fmt = rt.pickColorFormat(prefer, 3);
    if (!rt.createSwapchain(m.sc, fmt, sw.extent.width, sw.extent.height, 1,
                            XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT | XR_SWAPCHAIN_USAGE_MUTABLE_FORMAT_BIT))
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

// ---- stereo projection from the engine's eye render targets ----
struct Projection {
    xr::Swapchain sc;
    VkFormat srcFormat = VK_FORMAT_UNDEFINED;
    uint32_t w = 0, h = 0;
    bool blit = false;  // format conversion needed (float/other -> sRGB)
    bool failed = false;  // creation failed for failedW x failedH in failedFormat (another size or format is tried again)
    uint32_t failedW = 0, failedH = 0;
    VkFormat failedFormat = VK_FORMAT_UNDEFINED;
} pj;

// Lab: lab_eye_dump=<s> writes the left eye image once, <s> seconds after the first stereo frame, to
// ss4smvr/eye_dump.ppm (8-bit RGBA/BGRA eye formats), for image checks at full resolution (foveation, upscaling).
struct EyeDump {
    VkBuffer buf = VK_NULL_HANDLE;
    VkDeviceMemory mem = VK_NULL_HANDLE;
    void* ptr = nullptr;
    uint32_t w = 0, h = 0;
    int state = 0;  // 0 waiting, 1 copy recorded, 2 done
    ULONGLONG t0 = 0;
} g_dump;
bool g_dumpBgr;
bool dumpRecord(VkCommandBuffer cb, VkImage img, VkFormat fmt, uint32_t w, uint32_t h) {
    static const int after = se4::options::get("lab_eye_dump", 0);
    if (!after || g_dump.state || (fmt != VK_FORMAT_R8G8B8A8_UNORM && fmt != VK_FORMAT_B8G8R8A8_UNORM && fmt != VK_FORMAT_R8G8B8A8_SRGB &&
                                   fmt != VK_FORMAT_B8G8R8A8_SRGB))
        return false;
    if (!g_dump.t0) g_dump.t0 = GetTickCount64();
    if (GetTickCount64() - g_dump.t0 < ULONGLONG(after) * 1000) return false;
    VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bi.size = VkDeviceSize(w) * h * 4;
    bi.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    if (DFN(vkCreateBuffer)(g.device, &bi, nullptr, &g_dump.buf) != VK_SUCCESS) return g_dump.state = 2, false;
    VkMemoryRequirements mr;
    DFN(vkGetBufferMemoryRequirements)(g.device, g_dump.buf, &mr);
    VkPhysicalDeviceMemoryProperties mp;
    reinterpret_cast<PFN_vkGetPhysicalDeviceMemoryProperties>(realGIPA(g.instance, "vkGetPhysicalDeviceMemoryProperties"))(g.physical, &mp);
    const VkMemoryPropertyFlags want = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.allocationSize = mr.size;
    ai.memoryTypeIndex = UINT32_MAX;
    for (uint32_t i = 0; i < mp.memoryTypeCount && ai.memoryTypeIndex == UINT32_MAX; ++i)
        if ((mr.memoryTypeBits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & want) == want) ai.memoryTypeIndex = i;
    if (ai.memoryTypeIndex == UINT32_MAX || DFN(vkAllocateMemory)(g.device, &ai, nullptr, &g_dump.mem) != VK_SUCCESS) return g_dump.state = 2, false;
    DFN(vkBindBufferMemory)(g.device, g_dump.buf, g_dump.mem, 0);
    DFN(vkMapMemory)(g.device, g_dump.mem, 0, VK_WHOLE_SIZE, 0, &g_dump.ptr);
    VkBufferImageCopy c{};
    c.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    c.imageExtent = {w, h, 1};
    DFN(vkCmdCopyImageToBuffer)(cb, img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, g_dump.buf, 1, &c);
    g_dump.w = w, g_dump.h = h, g_dump.state = 1;
    g_dumpBgr = fmt == VK_FORMAT_B8G8R8A8_UNORM || fmt == VK_FORMAT_B8G8R8A8_SRGB;
    return true;
}
void dumpWrite(VkFence fence) {
    if (g_dump.state != 1) return;
    g_dump.state = 2;
    DFN(vkWaitForFences)(g.device, 1, &fence, VK_TRUE, UINT64_MAX);
    wchar_t path[MAX_PATH];
    se4::payloadDirectory(path, MAX_PATH - 20);
    wcscat(path, L"\\eye_dump.ppm");
    FILE* f = _wfopen(path, L"wb");
    if (!f) return;
    fprintf(f, "P6\n%u %u\n255\n", g_dump.w, g_dump.h);
    const uint8_t* p = static_cast<const uint8_t*>(g_dump.ptr);
    std::vector<uint8_t> row(size_t(g_dump.w) * 3);
    for (uint32_t y = 0; y < g_dump.h; ++y) {
        for (uint32_t x = 0; x < g_dump.w; ++x) {
            const uint8_t* q = p + (size_t(y) * g_dump.w + x) * 4;
            row[x * 3] = q[g_dumpBgr ? 2 : 0], row[x * 3 + 1] = q[1], row[x * 3 + 2] = q[g_dumpBgr ? 0 : 2];
        }
        fwrite(row.data(), 1, row.size(), f);
    }
    fclose(f);
    VRLOG("lab: left eye %ux%u written to eye_dump.ppm", g_dump.w, g_dump.h);
}

bool isUnorm8(VkFormat f) { return f == VK_FORMAT_R8G8B8A8_UNORM || f == VK_FORMAT_B8G8R8A8_UNORM || f == VK_FORMAT_A2B10G10R10_UNORM_PACK32; }

bool setupProjection(const eyes::Frame& f) {
    if (pj.sc.handle && pj.srcFormat == f.format && pj.w == f.width && pj.h == f.height) return true;
    if (pj.failed && pj.failedW == f.width && pj.failedH == f.height && pj.failedFormat == f.format) return false;
    pj.failed = false;
    if (pj.sc.handle) {
        vkq::Guard g;
        xrDestroySwapchain(pj.sc.handle);
        pj.sc = {};
    }
    auto& rt = xr::runtime();
    // 8-bit UNORM targets hold sRGB-encoded values: copy raw into the _SRGB twin. sRGB targets copy as is.
    // Anything else (e.g. float HDR) is blitted, which converts.
    int64_t twin = f.format == VK_FORMAT_B8G8R8A8_UNORM ? VK_FORMAT_B8G8R8A8_SRGB
                 : f.format == VK_FORMAT_R8G8B8A8_UNORM ? VK_FORMAT_R8G8B8A8_SRGB : int64_t(f.format);
    const int64_t prefer[] = {twin, VK_FORMAT_R8G8B8A8_SRGB, VK_FORMAT_B8G8R8A8_SRGB};
    int64_t fmt = rt.pickColorFormat(prefer, 3);
    pj.blit = fmt != twin;
    if (!rt.createSwapchain(pj.sc, fmt, f.width, f.height, 2, XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT |
                                                                 XR_SWAPCHAIN_USAGE_MUTABLE_FORMAT_BIT)) {
        pj.failed = true, pj.failedW = f.width, pj.failedH = f.height, pj.failedFormat = f.format;
        pj.w = pj.h = 0;
        return false;
    }
    pj.srcFormat = f.format, pj.w = f.width, pj.h = f.height;
    VRLOG("stereo: projection swapchain %ux%u x2, engine format %d -> %lld (%s)", f.width, f.height, int(f.format), (long long)fmt,
          pj.blit ? "blit" : "copy");
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

VkResult presentFrame(VkQueue q, const VkPresentInfoKHR* pi);
bool g_inputReleased;  // input already neutral while the session is not running (re-armed when it runs)
VKAPI_ATTR VkResult VKAPI_CALL hookQueuePresent(VkQueue q, const VkPresentInfoKHR* pi) {
    gputime::presentEntry();
    const VkResult r = presentFrame(q, pi);
    gputime::presentExit();
    return r;
}
VkResult presentFrame(VkQueue q, const VkPresentInfoKHR* pi) {
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
        if (!rt.running() && xr::input().ready()) {  // no frames, no input: release whatever was held
            if (!g_inputReleased) g_inputReleased = true, xr::input().neutral(), gamepad::publishFromXr(), vrdev::updateFromInput();
        }
        static unsigned reported;
        unsigned why = (!rt.running()) | (sw.images.empty() << 1) | (m.failed << 2) | ((q != g.xrQueue) << 3);
        if ((why & ~reported) && rt.running()) {
            reported |= why;
            VRLOG("mirror: passthrough present (reason mask %u, queue %p xrQueue %p)", why, (void*)q, (void*)g.xrQueue);
        }
        vkq::Guard lock;
        return realQueuePresent(q, pi);
    }
    focus::enable(g_gameWnd);
    if (!m.ready) {
        m.ready = setupMirror(sw);
        if (!m.ready) {
            m.failed = true;
            VRLOG("vk: mirror setup failed");
            vkq::Guard lock;
            return realQueuePresent(q, pi);
        }
    }

    static int traceFrames = 6;
    bool trace = traceFrames > 0 && traceFrames--;
#define TRACE(...) do { if (trace) VRLOG(__VA_ARGS__); } while (0)
    TRACE("mirror: present begin, waitFrame");
    XrFrameState fs;
    if (!rt.waitBeginFrame(fs)) {
        xr::input().neutral();
        gamepad::publishFromXr();
        vrdev::updateFromInput();
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

    TRACE("mirror: frame begun (shouldRender %d), acquire", fs.shouldRender);
    // The eye images submitted below were rendered with the views located at the previous present;
    // keep those for the projection layer before locating views for the next game frame.
    const xr::Views renderedWith = rt.views();
    // The hands the shown frame's guns were drawn with (the scope and ammo quads must sit on those guns).
    const XrPosef renderedGrip[2] = {xr::input().hand(0).grip, xr::input().hand(1).grip};
    // Poses located now render the NEXT game frame, which is displayed one period after this one: predict for that.
    const XrDuration period = fs.predictedDisplayPeriod > 0 && fs.predictedDisplayPeriod < 50000000 ? fs.predictedDisplayPeriod : 11111111;
    const XrTime nextDisplay = fs.predictedDisplayTime + period;
    rt.updateViews(nextDisplay);
    xr::input().sync(rt.session(), rt.playSpace(), nextDisplay);
    xr::input().locateGaze(rt.viewSpace(), nextDisplay);
    g_inputReleased = false;
    gamepad::publishFromXr();
    vrdev::updateFromInput();
    {
        static XrTime lastLog;
        if (fs.predictedDisplayTime - lastLog > 2000000000) {
            lastLog = fs.predictedDisplayTime;
            for (int h = 0; h < 2; ++h) {
                const auto& hs = xr::input().hand(h);
                VRLOG("input: %c active %d pos %.2f %.2f %.2f trig %.2f sq %.2f stick %.2f %.2f btn %x", h ? 'R' : 'L', hs.active,
                      hs.grip.position.x, hs.grip.position.y, hs.grip.position.z, hs.trigger, hs.squeeze, hs.stick.x, hs.stick.y, hs.buttons);
            }
        }
    }
    eyes::Frame ef = eyes::take();
    bool stereo = ef.ready && fs.shouldRender && renderedWith.valid && setupProjection(ef);
    uint32_t pidx = 0;
    if (stereo) stereo = rt.acquire(pj.sc, pidx);
    VkImage src = sw.images[pi->pImageIndices[0]];
    uint32_t idx = 0;
    bool haveImage = !stereo && fs.shouldRender && rt.acquire(m.sc, idx);
    // Scope view (engine eye 3) as a quad layer at the eyepiece of the weapon's hand.
    const eyes::ScopeFrame scopeFrame = eyes::takeScope();
    const scope::Layer scopeLayer = scope::layer(renderedGrip[1], renderedGrip[0]);
    const bool scopeOn = stereo && scopeLayer.visible && scopelayer::begin(scopeFrame);
    const bool ammoOn = stereo && ammolayer::begin() > 0;
    TRACE("mirror: acquired %d idx %u, submit", haveImage, idx);
    VkSemaphore waitOn = VK_NULL_HANDLE;
    VkResult result;
    {
        // Recording needs no queue lock (and fov::record takes foveation's lock, which is held while foveation
        // itself submits under the queue lock: taking them in the other order here could deadlock).
        int s = m.slot;
        m.slot = (m.slot + 1) % Mirror::kRing;
        DFN(vkWaitForFences)(g.device, 1, &m.fence[s], VK_TRUE, UINT64_MAX);
        DFN(vkResetFences)(g.device, 1, &m.fence[s]);
        VkCommandBuffer cb = m.cmd[s];
        DFN(vkResetCommandBuffer)(cb, 0);
        VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        DFN(vkBeginCommandBuffer)(cb, &bi);
        fov::record(cb);
        if (haveImage) {
            VkImage dst = m.sc.images[idx];
            imageBarrier(cb, src, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, 0, VK_ACCESS_TRANSFER_READ_BIT,
                         VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
            imageBarrier(cb, dst, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT,
                         VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
            if (sw.extent.width == m.sc.width && sw.extent.height == m.sc.height) {
                // Raw copy: the game writes sRGB-encoded values into a UNORM swapchain; the XR swapchain is
                // the matching _SRGB format, so bytes must not be re-encoded (a blit would brighten them).
                VkImageCopy c{};
                c.srcSubresource = c.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
                c.extent = {sw.extent.width, sw.extent.height, 1};
                DFN(vkCmdCopyImage)(cb, src, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &c);
            } else {
                VkImageBlit blit{};
                blit.srcSubresource = blit.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
                blit.srcOffsets[1] = {int32_t(sw.extent.width), int32_t(sw.extent.height), 1};
                blit.dstOffsets[1] = {int32_t(m.sc.width), int32_t(m.sc.height), 1};
                DFN(vkCmdBlitImage)(cb, src, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, VK_FILTER_LINEAR);
            }
            imageBarrier(cb, src, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_ACCESS_TRANSFER_READ_BIT, 0,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);
            imageBarrier(cb, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT,
                         VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);
        }
        if (stereo) {
            VkImage dst = pj.sc.images[pidx];
            VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
            b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            b.image = dst;
            b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 2};
            b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            b.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            DFN(vkCmdPipelineBarrier)(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);
            for (uint32_t e = 0; e < 2; ++e) {
                // The engine leaves submitted eye textures in TRANSFER_SRC_OPTIMAL in its Vulkan mode
                // (gfx vt+0x288 transition before SubmitEye, same contract as OpenVR's Vulkan submit).
                if (pj.blit) {
                    VkImageBlit bl{};
                    bl.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
                    bl.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, e, 1};
                    bl.srcOffsets[1] = bl.dstOffsets[1] = {int32_t(pj.w), int32_t(pj.h), 1};
                    DFN(vkCmdBlitImage)(cb, ef.image[e], VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &bl,
                                        VK_FILTER_NEAREST);
                } else {
                    VkImageCopy c{};
                    c.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
                    c.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, e, 1};
                    c.extent = {pj.w, pj.h, 1};
                    DFN(vkCmdCopyImage)(cb, ef.image[e], VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &c);
                }
            }
            dumpRecord(cb, ef.image[0], pj.srcFormat, pj.w, pj.h);
            b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            b.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            b.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
            DFN(vkCmdPipelineBarrier)(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);
        }
        if (scopeOn) scopelayer::record(cb, scopeFrame, scopeLayer.reticle);
        if (ammoOn) ammolayer::record(cb, s);
        DFN(vkEndCommandBuffer)(cb);
        VkPipelineStageFlags stagesFixed[8];
        std::vector<VkPipelineStageFlags> stagesMore;
        VkPipelineStageFlags* stages = stagesFixed;
        if (pi->waitSemaphoreCount > 8) stagesMore.resize(pi->waitSemaphoreCount), stages = stagesMore.data();
        for (uint32_t i = 0; i < pi->waitSemaphoreCount; ++i) stages[i] = VK_PIPELINE_STAGE_TRANSFER_BIT;
        VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        si.waitSemaphoreCount = pi->waitSemaphoreCount;
        si.pWaitSemaphores = pi->pWaitSemaphores;
        si.pWaitDstStageMask = stages;
        si.commandBufferCount = 1;
        si.pCommandBuffers = &cb;
        si.signalSemaphoreCount = 1;
        si.pSignalSemaphores = &m.done[s];
        vkq::Guard lock;
        if (const VkResult sr = realQueueSubmit(q, 1, &si, m.fence[s]); sr != VK_SUCCESS) {
            static int logged;
            if (logged++ < 3) VRLOG("mirror: queue submit failed (%d)", int(sr));
        }
        dumpWrite(m.fence[s]);
        waitOn = m.done[s];
        // No CPU wait: the blit is submitted on the session's queue before xrReleaseSwapchainImage, and the
        // runtime orders its own work on that queue after it (OpenXR Vulkan swapchain rules).
        VkPresentInfoKHR p = *pi;
        p.waitSemaphoreCount = 1;
        p.pWaitSemaphores = &waitOn;
        TRACE("mirror: blit done, present");
        result = realQueuePresent(q, &p);
    }
    TRACE("mirror: presented %d, release", int(result));
    if (haveImage) rt.release(m.sc);
    if (stereo) rt.release(pj.sc);
    if (scopeOn) scopelayer::end();
    if (ammoOn) ammolayer::end();
    TRACE("mirror: endFrame");

    // Projection layer: the engine rendered each eye with that eye's horizontal FOV and the vertical FOV
    // shared by both eyes (device slot 0x070), at the head/eye poses it read this frame.
    XrCompositionLayerProjectionView pv[2] = {{XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW}, {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW}};
    XrCompositionLayerProjection proj{XR_TYPE_COMPOSITION_LAYER_PROJECTION};
    if (stereo) {
        const auto& v = renderedWith;
        float up = std::max(v.fov[0].angleUp, v.fov[1].angleUp), down = std::min(v.fov[0].angleDown, v.fov[1].angleDown);
        for (int e = 0; e < 2; ++e) {
            core::Pose head{{v.head.orientation.x, v.head.orientation.y, v.head.orientation.z, v.head.orientation.w},
                            {v.head.position.x, v.head.position.y, v.head.position.z}};
            const XrPosef& ev = v.inView[e].pose;
            core::Pose eye = core::compose(head, {{ev.orientation.x, ev.orientation.y, ev.orientation.z, ev.orientation.w},
                                                  {ev.position.x, ev.position.y, ev.position.z}});
            pv[e].pose.orientation = {eye.q.x, eye.q.y, eye.q.z, eye.q.w};
            pv[e].pose.position = {eye.p.x, eye.p.y, eye.p.z};
            pv[e].fov = {v.fov[e].angleLeft, v.fov[e].angleRight, up, down};
            pv[e].subImage.swapchain = pj.sc.handle;
            pv[e].subImage.imageArrayIndex = uint32_t(e);
            pv[e].subImage.imageRect = {{0, 0}, {int32_t(pj.w), int32_t(pj.h)}};
        }
        proj.space = rt.playSpace();
        proj.viewCount = 2;
        proj.views = pv;
        static bool announced;
        if (!announced) VRLOG("stereo: submitting engine-rendered eyes as the projection layer");
        announced = true;
    }

    XrCompositionLayerQuad quad{XR_TYPE_COMPOSITION_LAYER_QUAD};
    quad.space = rt.localSpace();
    quad.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
    quad.subImage.swapchain = m.sc.handle;
    quad.subImage.imageRect = {{0, 0}, {int32_t(m.sc.width), int32_t(m.sc.height)}};
    quad.pose = m.pose;
    float w = 3.2f;
    quad.size = {w, w * float(m.sc.height) / float(m.sc.width)};
    XrCompositionLayerQuad scopeQuad{XR_TYPE_COMPOSITION_LAYER_QUAD};
    if (scopeOn) {
        static const bool opaque = se4::options::get("scope_opaque", 0) != 0;  // lab: show the square image without its mask
        scopeQuad.layerFlags = opaque ? 0 : XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;  // premultiplied (scope.comp)
        scopeQuad.space = rt.playSpace();
        scopeQuad.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
        scopeQuad.subImage.swapchain = scopelayer::swapchain();
        scopeQuad.subImage.imageRect = {{0, 0}, {int32_t(scopelayer::size()), int32_t(scopelayer::size())}};
        scopeQuad.pose = scopeLayer.pose;
        scopeQuad.size = {scopeLayer.size, scopeLayer.size};
        static int logged;
        if (logged++ % 300 == 0) {
            const auto& hp = rt.views().head.position;
            const auto& p = scopeQuad.pose;
            VRLOG("scope: quad at %.3f %.3f %.3f q %.3f %.3f %.3f %.3f size %.3f (head %.3f %.3f %.3f)", p.position.x, p.position.y,
                  p.position.z, p.orientation.x, p.orientation.y, p.orientation.z, p.orientation.w, scopeQuad.size.width, hp.x, hp.y, hp.z);
        }
    }
    XrCompositionLayerQuad ammoQuads[2];
    const int ammoN = ammoOn ? ammolayer::layers(rt.playSpace(), ammoQuads, renderedGrip) : 0;
    const XrCompositionLayerBaseHeader* layers[4] = {stereo ? reinterpret_cast<XrCompositionLayerBaseHeader*>(&proj)
                                                            : reinterpret_cast<XrCompositionLayerBaseHeader*>(&quad)};
    uint32_t layerCount = 1;
    if (scopeOn) layers[layerCount++] = reinterpret_cast<XrCompositionLayerBaseHeader*>(&scopeQuad);
    for (int i = 0; i < ammoN; ++i) layers[layerCount++] = reinterpret_cast<XrCompositionLayerBaseHeader*>(&ammoQuads[i]);
    if (ammoN) {
        static int logged;
        if (!logged++) {  // the first frame's quad, for lab checks
            const auto& hp = rt.views().head.position;
            const auto& p = ammoQuads[0].pose;
            VRLOG("ammo: %d quad(s), first at %.3f %.3f %.3f q %.3f %.3f %.3f %.3f size %.3f (head %.3f %.3f %.3f)", ammoN, p.position.x,
                  p.position.y, p.position.z, p.orientation.x, p.orientation.y, p.orientation.z, p.orientation.w, ammoQuads[0].size.width, hp.x,
                  hp.y, hp.z);
        }
    }
    bool ended = rt.endFrame(fs, layers, (stereo || (haveImage && m.placed)) ? layerCount : 0);
    static uint32_t frames, imaged, submitted;
    ++frames;
    imaged += haveImage;
    submitted += ended && haveImage && m.placed && fs.shouldRender;
    static XrTime windowStart;
    static uint32_t windowFrames;
    if (!windowStart) windowStart = fs.predictedDisplayTime;
    ++windowFrames;
    if (fs.predictedDisplayTime - windowStart >= 5000000000LL) {
        double secs = double(fs.predictedDisplayTime - windowStart) / 1e9;
        VRLOG("mirror: %.1f fps (frames %u submitted %u, display period %.2f ms)", windowFrames / secs, frames, submitted,
              fs.predictedDisplayPeriod / 1e6);
        windowStart = fs.predictedDisplayTime;
        gputime::log(windowFrames);
        windowFrames = 0;
        gamepad::logStats();
        vrdev::logStats();
    }
    return result;
}

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL hookGDPA(VkDevice d, const char* name);

PFN_vkVoidFunction deviceOverride(const char* name) {
    // Render-pass observer in front of foveation's pass hooks (both chain to the next layer's function).
    if (!strcmp(name, "vkCmdEndRenderPass")) {
        realEndPass = DFN(vkCmdEndRenderPass);
        return reinterpret_cast<PFN_vkVoidFunction>(hookEndPass);
    }
    if (!strcmp(name, "vkCmdEndRenderPass2") || !strcmp(name, "vkCmdEndRenderPass2KHR")) {
        realEndPass2 = reinterpret_cast<PFN_vkCmdEndRenderPass2>(realGDPA(g.device, name));
        if (!realEndPass2) return nullptr;
        return reinterpret_cast<PFN_vkVoidFunction>(hookEndPass2);
    }
    if (up::wanted() && !strcmp(name, "vkCmdSetViewport")) {  // the scene depth partition, for the motion vectors
        realSetViewport = DFN(vkCmdSetViewport);
        return reinterpret_cast<PFN_vkVoidFunction>(hookSetViewport);
    }
    if (upint::passTraceOn() && !strcmp(name, "vkCreateImageView")) {
        realCreateView = DFN(vkCreateImageView);
        return reinterpret_cast<PFN_vkVoidFunction>(hookCreateView);
    }
    if (upint::passTraceOn() && !strcmp(name, "vkCreateFramebuffer")) {
        realCreateFb = DFN(vkCreateFramebuffer);
        return reinterpret_cast<PFN_vkVoidFunction>(hookCreateFb);
    }
    if (!strcmp(name, "vkCmdBeginRenderPass")) {
        auto f = fov::override(name);
        nextBeginPass = f ? reinterpret_cast<PFN_vkCmdBeginRenderPass>(f) : DFN(vkCmdBeginRenderPass);
        return reinterpret_cast<PFN_vkVoidFunction>(hookBeginPassObs);
    }
    if (!strcmp(name, "vkCmdBeginRenderPass2") || !strcmp(name, "vkCmdBeginRenderPass2KHR")) {
        auto f = fov::override(name);
        nextBeginPass2 = f ? reinterpret_cast<PFN_vkCmdBeginRenderPass2>(f) : reinterpret_cast<PFN_vkCmdBeginRenderPass2>(realGDPA(g.device, name));
        if (!nextBeginPass2) return nullptr;
        return reinterpret_cast<PFN_vkVoidFunction>(hookBeginPass2Obs);
    }
    if (!strcmp(name, "vkCreateRenderPass")) {
        auto f = fov::override(name);
        nextCreateRenderPass = f ? reinterpret_cast<PFN_vkCreateRenderPass>(f) : DFN(vkCreateRenderPass);
        return reinterpret_cast<PFN_vkVoidFunction>(hookCreateRenderPassObs);
    }
    if (!strcmp(name, "vkCreateRenderPass2") || !strcmp(name, "vkCreateRenderPass2KHR")) {
        auto f = fov::override(name);
        nextCreateRenderPass2 = f ? reinterpret_cast<PFN_vkCreateRenderPass2>(f) : reinterpret_cast<PFN_vkCreateRenderPass2>(realGDPA(g.device, name));
        if (!nextCreateRenderPass2) return nullptr;
        return reinterpret_cast<PFN_vkVoidFunction>(hookCreateRenderPass2Obs);
    }
    if (auto f = fov::override(name)) return f;
    if (!strcmp(name, "vkQueueSubmit")) return reinterpret_cast<PFN_vkVoidFunction>(hookQueueSubmit);
    if (!strcmp(name, "vkQueueSubmit2") || !strcmp(name, "vkQueueSubmit2KHR")) return reinterpret_cast<PFN_vkVoidFunction>(hookQueueSubmit2);
    if (!strcmp(name, "vkQueuePresentKHR")) return reinterpret_cast<PFN_vkVoidFunction>(hookQueuePresent);
    if (!strcmp(name, "vkQueueWaitIdle")) return reinterpret_cast<PFN_vkVoidFunction>(hookQueueWaitIdle);
    if (!strcmp(name, "vkDeviceWaitIdle")) return reinterpret_cast<PFN_vkVoidFunction>(hookDeviceWaitIdle);
    if (!strcmp(name, "vkCreateSwapchainKHR")) return reinterpret_cast<PFN_vkVoidFunction>(hookCreateSwapchain);
    if (!strcmp(name, "vkCreateImage")) return reinterpret_cast<PFN_vkVoidFunction>(hookCreateImage);
    if (!strcmp(name, "vkBeginCommandBuffer")) return reinterpret_cast<PFN_vkVoidFunction>(hookBeginCB);
    if (!strcmp(name, "vkCmdPipelineBarrier")) return reinterpret_cast<PFN_vkVoidFunction>(hookBarrier);
    if (realBarrier2 && (!strcmp(name, "vkCmdPipelineBarrier2") || !strcmp(name, "vkCmdPipelineBarrier2KHR")))
        return reinterpret_cast<PFN_vkVoidFunction>(hookBarrier2);
    if (!strcmp(name, "vkDestroyImage")) return reinterpret_cast<PFN_vkVoidFunction>(hookDestroyImage);
    if (!strcmp(name, "vkGetDeviceProcAddr")) return reinterpret_cast<PFN_vkVoidFunction>(hookGDPA);
    return nullptr;
}

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL hookGDPA(VkDevice d, const char* name) {
    PFN_vkVoidFunction real = realGDPA(d, name);
    if (!real || !name || d != g.device || !active()) return real;
    if (auto o = deviceOverride(name)) return o;
    return real;
}

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL hookGIPA(VkInstance inst, const char* name) {
    if (name) {
        if (!strcmp(name, "vkCreateInstance")) return reinterpret_cast<PFN_vkVoidFunction>(hookCreateInstance);
        if (!strcmp(name, "vkCreateDevice")) return reinterpret_cast<PFN_vkVoidFunction>(hookCreateDevice);
        if (!strcmp(name, "vkCreateWin32SurfaceKHR") && inst) {
            realCreateWin32Surface = reinterpret_cast<PFN_vkCreateWin32SurfaceKHR>(realGIPA(inst, name));
            return realCreateWin32Surface ? reinterpret_cast<PFN_vkVoidFunction>(hookCreateWin32Surface) : nullptr;
        }
        if (!strcmp(name, "vkGetDeviceProcAddr")) {
            if (!realGDPA) realGDPA = reinterpret_cast<PFN_vkGetDeviceProcAddr>(realGIPA(inst, name));
            return realGDPA ? reinterpret_cast<PFN_vkVoidFunction>(hookGDPA) : nullptr;
        }
        if (inst && inst == g.instance && active())
            if (auto o = deviceOverride(name)) return o;
    }
    return realGIPA(inst, name);
}

}  // namespace

// The engine loads vulkan-1.dll dynamically and resolves entry points with GetProcAddress (game-module
// callers only, see se4/procaddr). Wrappers are handed back without patching vulkan-1.dll.
FARPROC interceptProc(HMODULE mod, const char* name, FARPROC real) {
    if (mod != GetModuleHandleW(L"vulkan-1.dll")) return real;
    vkq::initLock();
    if (!strcmp(name, "vkGetInstanceProcAddr")) {
        realGIPA = reinterpret_cast<PFN_vkGetInstanceProcAddr>(real);
        // The Vulkan backend is initialising on a game thread: bring up OpenXR and install the VR device
        // now, because the backend queries the device during init (R1: install before gfx init).
        if (se4::g_vrEnabled && !(se4::g_debugSkip & 8) && xr::runtime().ensure()) vrdev::install();
        return reinterpret_cast<FARPROC>(hookGIPA);
    }
    if (!strcmp(name, "vkCreateInstance")) {
        realCreateInstance = reinterpret_cast<PFN_vkCreateInstance>(real);
        return reinterpret_cast<FARPROC>(hookCreateInstance);
    }
    if (!strcmp(name, "vkCreateDevice")) {
        realCreateDevice = reinterpret_cast<PFN_vkCreateDevice>(real);
        return reinterpret_cast<FARPROC>(hookCreateDevice);
    }
    if (!strcmp(name, "vkGetDeviceProcAddr")) {
        realGDPA = reinterpret_cast<PFN_vkGetDeviceProcAddr>(real);
        return reinterpret_cast<FARPROC>(hookGDPA);
    }
    VRLOG("vk: game resolved %s by export (not intercepted)", name);
    return real;
}

}  // namespace vk_layer
