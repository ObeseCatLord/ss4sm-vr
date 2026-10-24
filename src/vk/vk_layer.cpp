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
#include "xr/input.hpp"
#include <windows.h>
#include <MinHook.h>
#include <intrin.h>
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
    if (!realCreateInstance) realCreateInstance = reinterpret_cast<PFN_vkCreateInstance>(realGIPA(nullptr, "vkCreateInstance"));
    if (g_redirectThread == GetCurrentThreadId() || g.instance || !callerInGame(ret)) return realCreateInstance(ci, a, out);
    onGameVulkan();
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
    if (!realCreateDevice) realCreateDevice = reinterpret_cast<PFN_vkCreateDevice>(realGIPA(g.instance, "vkCreateDevice"));
    if (!realGDPA) realGDPA = reinterpret_cast<PFN_vkGetDeviceProcAddr>(realGIPA(g.instance, "vkGetDeviceProcAddr"));
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
        static unsigned reported;
        unsigned why = (!rt.running()) | (sw.images.empty() << 1) | (m.failed << 2) | ((q != g.xrQueue) << 3);
        if ((why & ~reported) && rt.running()) {
            reported |= why;
            VRLOG("mirror: passthrough present (reason mask %u, queue %p xrQueue %p)", why, (void*)q, (void*)g.xrQueue);
        }
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

    static int traceFrames = 6;
    bool trace = traceFrames > 0 && traceFrames--;
#define TRACE(...) do { if (trace) VRLOG(__VA_ARGS__); } while (0)
    TRACE("mirror: present begin, waitFrame");
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

    TRACE("mirror: frame begun (shouldRender %d), acquire", fs.shouldRender);
    xr::input().sync(rt.session(), rt.localSpace(), fs.predictedDisplayTime);
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
    VkImage src = sw.images[pi->pImageIndices[0]];
    uint32_t idx = 0;
    bool haveImage = rt.acquire(m.sc, idx);
    TRACE("mirror: acquired %d idx %u, submit", haveImage, idx);
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
    TRACE("mirror: endFrame");

    XrCompositionLayerQuad quad{XR_TYPE_COMPOSITION_LAYER_QUAD};
    quad.space = rt.localSpace();
    quad.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
    quad.subImage.swapchain = m.sc.handle;
    quad.subImage.imageRect = {{0, 0}, {int32_t(m.sc.width), int32_t(m.sc.height)}};
    quad.pose = m.pose;
    float w = 3.2f;
    quad.size = {w, w * float(m.sc.height) / float(m.sc.width)};
    const XrCompositionLayerBaseHeader* layers[] = {reinterpret_cast<XrCompositionLayerBaseHeader*>(&quad)};
    bool ended = rt.endFrame(fs, layers, (haveImage && m.placed) ? 1 : 0);
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
        windowFrames = 0;
    }
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
        if (!strcmp(name, "vkGetDeviceProcAddr")) {
            if (!realGDPA) realGDPA = reinterpret_cast<PFN_vkGetDeviceProcAddr>(realGIPA(inst, name));
            return realGDPA ? reinterpret_cast<PFN_vkVoidFunction>(hookGDPA) : nullptr;
        }
        if (inst && inst == g.instance && g.xrDevice)
            if (auto o = deviceOverride(name)) return o;
    }
    return realGIPA(inst, name);
}

// The engine loads vulkan-1.dll dynamically and resolves entry points with GetProcAddress. Hooking
// GetProcAddress (installed once at attach) lets us hand back wrappers without patching vulkan-1.dll or
// doing anything under the loader lock. Calls from outside the game module are untouched.
using GetProcAddressFn = FARPROC(WINAPI*)(HMODULE, LPCSTR);
GetProcAddressFn realGetProcAddress;

FARPROC WINAPI hookGetProcAddress(HMODULE mod, LPCSTR name) {
    FARPROC real = realGetProcAddress(mod, name);
    if (!real || reinterpret_cast<uintptr_t>(name) < 0x10000 || !callerInGame(__builtin_return_address(0))) return real;
    if (name[0] != 'v' || name[1] != 'k' || mod != GetModuleHandleW(L"vulkan-1.dll")) return real;
    if (!strcmp(name, "vkGetInstanceProcAddr")) {
        realGIPA = reinterpret_cast<PFN_vkGetInstanceProcAddr>(real);
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
}  // namespace

bool install() {
    vkq::initLock();
    void* target = reinterpret_cast<void*>(GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "GetProcAddress"));
    if (!target || MH_CreateHook(target, reinterpret_cast<void*>(hookGetProcAddress), reinterpret_cast<void**>(&realGetProcAddress)) != MH_OK ||
        MH_EnableHook(target) != MH_OK) {
        VRLOG("vk: GetProcAddress hook FAILED");
        return false;
    }
    VRLOG("vk: GetProcAddress hook installed");
    return true;
}

}  // namespace vk_layer
