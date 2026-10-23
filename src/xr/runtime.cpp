#include "xr/runtime.hpp"
#include "se4/log.hpp"
#include "vk/queue_lock.hpp"
#include <cstring>

namespace xr {
namespace {
Runtime g_runtime;

template <class T> T fn(XrInstance inst, const char* name) {
    PFN_xrVoidFunction f = nullptr;
    xrGetInstanceProcAddr(inst, name, &f);
    return reinterpret_cast<T>(f);
}

bool check(XrResult r, const char* what) {
    if (XR_SUCCEEDED(r)) return true;
    VRLOG("xr: %s failed: %s (%d)", what, resultName(r), int(r));
    return false;
}
}  // namespace

Runtime& runtime() { return g_runtime; }

const char* resultName(XrResult r) {
    static char buf[XR_MAX_RESULT_STRING_SIZE];
    if (g_runtime.instance() && XR_SUCCEEDED(xrResultToString(g_runtime.instance(), r, buf))) return buf;
    return "XR_?";
}

bool Runtime::initialize() {
    uint32_t n = 0;
    if (!check(xrEnumerateInstanceExtensionProperties(nullptr, 0, &n, nullptr), "enumerate extensions")) return false;
    std::vector<XrExtensionProperties> exts(n, {XR_TYPE_EXTENSION_PROPERTIES});
    xrEnumerateInstanceExtensionProperties(nullptr, n, &n, exts.data());
    auto has = [&](const char* e) {
        for (auto& x : exts)
            if (!strcmp(x.extensionName, e)) return true;
        return false;
    };
    if (!has(XR_KHR_VULKAN_ENABLE2_EXTENSION_NAME)) {
        VRLOG("xr: runtime lacks XR_KHR_vulkan_enable2");
        return false;
    }
    std::vector<const char*> want = {XR_KHR_VULKAN_ENABLE2_EXTENSION_NAME};
    for (const char* e : {XR_KHR_VISIBILITY_MASK_EXTENSION_NAME, XR_KHR_COMPOSITION_LAYER_CYLINDER_EXTENSION_NAME,
                          XR_KHR_COMPOSITION_LAYER_DEPTH_EXTENSION_NAME, XR_EXT_HAND_TRACKING_EXTENSION_NAME})
        if (has(e)) want.push_back(e);

    XrInstanceCreateInfo ci{XR_TYPE_INSTANCE_CREATE_INFO};
    strcpy(ci.applicationInfo.applicationName, "Serious Sam 4 VR");
    strcpy(ci.applicationInfo.engineName, "Serious Engine 4");
    ci.applicationInfo.apiVersion = XR_API_VERSION_1_0;
    ci.enabledExtensionCount = uint32_t(want.size());
    ci.enabledExtensionNames = want.data();
    if (!check(xrCreateInstance(&ci, &instance_), "xrCreateInstance")) return false;
    XrInstanceProperties ip{XR_TYPE_INSTANCE_PROPERTIES};
    xrGetInstanceProperties(instance_, &ip);
    VRLOG("xr: runtime %s %u.%u.%u", ip.runtimeName, XR_VERSION_MAJOR(ip.runtimeVersion), XR_VERSION_MINOR(ip.runtimeVersion),
          XR_VERSION_PATCH(ip.runtimeVersion));

    XrSystemGetInfo sgi{XR_TYPE_SYSTEM_GET_INFO};
    sgi.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
    XrSystemId sys = XR_NULL_SYSTEM_ID;
    if (!check(xrGetSystem(instance_, &sgi, &sys), "xrGetSystem")) return false;
    XrSystemProperties sp{XR_TYPE_SYSTEM_PROPERTIES};
    xrGetSystemProperties(instance_, sys, &sp);
    VRLOG("xr: system '%s'", sp.systemName);

    getRequirements_ = fn<PFN_xrGetVulkanGraphicsRequirements2KHR>(instance_, "xrGetVulkanGraphicsRequirements2KHR");
    createInstance_ = fn<PFN_xrCreateVulkanInstanceKHR>(instance_, "xrCreateVulkanInstanceKHR");
    createDevice_ = fn<PFN_xrCreateVulkanDeviceKHR>(instance_, "xrCreateVulkanDeviceKHR");
    getDevice_ = fn<PFN_xrGetVulkanGraphicsDevice2KHR>(instance_, "xrGetVulkanGraphicsDevice2KHR");
    XrGraphicsRequirementsVulkan2KHR req{XR_TYPE_GRAPHICS_REQUIREMENTS_VULKAN2_KHR};
    if (!getRequirements_ || !check(getRequirements_(instance_, sys, &req), "graphics requirements")) return false;
    system_ = sys;
    return true;
}

VkResult Runtime::createVulkanInstance(PFN_vkGetInstanceProcAddr gipa, const VkInstanceCreateInfo* ci, VkInstance* out) {
    XrVulkanInstanceCreateInfoKHR x{XR_TYPE_VULKAN_INSTANCE_CREATE_INFO_KHR};
    x.systemId = system_;
    x.pfnGetInstanceProcAddr = gipa;
    x.vulkanCreateInfo = ci;
    VkResult vr = VK_ERROR_INITIALIZATION_FAILED;
    if (!check(createInstance_(instance_, &x, out, &vr), "xrCreateVulkanInstanceKHR")) return VK_ERROR_INITIALIZATION_FAILED;
    return vr;
}

VkPhysicalDevice Runtime::requiredPhysicalDevice(VkInstance inst) {
    XrVulkanGraphicsDeviceGetInfoKHR gi{XR_TYPE_VULKAN_GRAPHICS_DEVICE_GET_INFO_KHR};
    gi.systemId = system_;
    gi.vulkanInstance = inst;
    VkPhysicalDevice pd = VK_NULL_HANDLE;
    check(getDevice_(instance_, &gi, &pd), "xrGetVulkanGraphicsDevice2KHR");
    return pd;
}

VkResult Runtime::createVulkanDevice(PFN_vkGetInstanceProcAddr gipa, VkPhysicalDevice pd, const VkDeviceCreateInfo* ci, VkDevice* out) {
    XrVulkanDeviceCreateInfoKHR x{XR_TYPE_VULKAN_DEVICE_CREATE_INFO_KHR};
    x.systemId = system_;
    x.pfnGetInstanceProcAddr = gipa;
    x.vulkanPhysicalDevice = pd;
    x.vulkanCreateInfo = ci;
    VkResult vr = VK_ERROR_INITIALIZATION_FAILED;
    if (!check(createDevice_(instance_, &x, out, &vr), "xrCreateVulkanDeviceKHR")) return VK_ERROR_INITIALIZATION_FAILED;
    return vr;
}

bool Runtime::createSession(VkInstance inst, VkPhysicalDevice pd, VkDevice dev, uint32_t family, uint32_t index) {
    XrGraphicsBindingVulkan2KHR gb{XR_TYPE_GRAPHICS_BINDING_VULKAN2_KHR};
    gb.instance = inst;
    gb.physicalDevice = pd;
    gb.device = dev;
    gb.queueFamilyIndex = family;
    gb.queueIndex = index;
    XrSessionCreateInfo sci{XR_TYPE_SESSION_CREATE_INFO};
    sci.next = &gb;
    sci.systemId = system_;
    {
        vkq::Guard g;
        if (!check(xrCreateSession(instance_, &sci, &session_), "xrCreateSession")) return false;
    }
    XrReferenceSpaceCreateInfo rs{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
    rs.poseInReferenceSpace.orientation.w = 1;
    rs.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
    check(xrCreateReferenceSpace(session_, &rs, &local_), "LOCAL space");
    rs.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_VIEW;
    check(xrCreateReferenceSpace(session_, &rs, &view_), "VIEW space");
    uint32_t n = 0;
    xrEnumerateSwapchainFormats(session_, 0, &n, nullptr);
    formats_.resize(n);
    xrEnumerateSwapchainFormats(session_, n, &n, formats_.data());
    VRLOG("xr: session created on queue family %u index %u, %u swapchain formats", family, index, n);
    return true;
}

void Runtime::pollEvents() {
    if (!instance_) return;
    XrEventDataBuffer ev{XR_TYPE_EVENT_DATA_BUFFER};
    while (xrPollEvent(instance_, &ev) == XR_SUCCESS) {
        if (ev.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED) {
            auto* s = reinterpret_cast<XrEventDataSessionStateChanged*>(&ev);
            state_ = s->state;
            VRLOG("xr: session state %d", int(state_));
            if (state_ == XR_SESSION_STATE_READY) {
                XrSessionBeginInfo bi{XR_TYPE_SESSION_BEGIN_INFO};
                bi.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
                vkq::Guard g;
                running_ = check(xrBeginSession(session_, &bi), "xrBeginSession");
            } else if (state_ == XR_SESSION_STATE_STOPPING) {
                vkq::Guard g;
                check(xrEndSession(session_), "xrEndSession");
                running_ = false;
            } else if (state_ == XR_SESSION_STATE_EXITING || state_ == XR_SESSION_STATE_LOSS_PENDING) {
                running_ = false;
            }
        } else if (ev.type == XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING) {
            VRLOG("xr: instance loss pending");
            running_ = false;
        }
        ev = {XR_TYPE_EVENT_DATA_BUFFER};
    }
}

int64_t Runtime::pickColorFormat(const int64_t* preferred, size_t n) const {
    for (size_t i = 0; i < n; ++i)
        for (int64_t f : formats_)
            if (f == preferred[i]) return f;
    return formats_.empty() ? 0 : formats_[0];
}

bool Runtime::createSwapchain(Swapchain& sc, int64_t format, uint32_t w, uint32_t h, uint32_t layers, XrSwapchainUsageFlags usage) {
    XrSwapchainCreateInfo ci{XR_TYPE_SWAPCHAIN_CREATE_INFO};
    ci.usageFlags = usage;
    ci.format = format;
    ci.sampleCount = 1;
    ci.width = w;
    ci.height = h;
    ci.faceCount = 1;
    ci.arraySize = layers;
    ci.mipCount = 1;
    vkq::Guard g;
    if (!check(xrCreateSwapchain(session_, &ci, &sc.handle), "xrCreateSwapchain")) return false;
    uint32_t n = 0;
    xrEnumerateSwapchainImages(sc.handle, 0, &n, nullptr);
    std::vector<XrSwapchainImageVulkan2KHR> imgs(n, {XR_TYPE_SWAPCHAIN_IMAGE_VULKAN2_KHR});
    xrEnumerateSwapchainImages(sc.handle, n, &n, reinterpret_cast<XrSwapchainImageBaseHeader*>(imgs.data()));
    sc.images.clear();
    for (auto& i : imgs) sc.images.push_back(i.image);
    sc.format = format;
    sc.width = w;
    sc.height = h;
    sc.layers = layers;
    VRLOG("xr: swapchain %ux%u x%u format %lld, %u images", w, h, layers, (long long)format, n);
    return true;
}

bool Runtime::acquire(Swapchain& sc, uint32_t& index) {
    vkq::Guard g;
    if (!check(xrAcquireSwapchainImage(sc.handle, nullptr, &index), "xrAcquireSwapchainImage")) return false;
    XrSwapchainImageWaitInfo wi{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
    wi.timeout = 100000000;  // 100 ms
    return check(xrWaitSwapchainImage(sc.handle, &wi), "xrWaitSwapchainImage");
}

void Runtime::release(Swapchain& sc) {
    vkq::Guard g;
    check(xrReleaseSwapchainImage(sc.handle, nullptr), "xrReleaseSwapchainImage");
}

bool Runtime::waitBeginFrame(XrFrameState& fs) {
    fs = {XR_TYPE_FRAME_STATE};
    if (!check(xrWaitFrame(session_, nullptr, &fs), "xrWaitFrame")) return false;
    vkq::Guard g;
    return check(xrBeginFrame(session_, nullptr), "xrBeginFrame");
}

bool Runtime::endFrame(const XrFrameState& fs, const XrCompositionLayerBaseHeader* const* layers, uint32_t count) {
    XrFrameEndInfo ei{XR_TYPE_FRAME_END_INFO};
    ei.displayTime = fs.predictedDisplayTime;
    ei.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
    ei.layerCount = fs.shouldRender ? count : 0;
    ei.layers = layers;
    vkq::Guard g;
    return check(xrEndFrame(session_, &ei), "xrEndFrame");
}

bool Runtime::locateHead(XrTime t, XrPosef& head) {
    XrSpaceLocation loc{XR_TYPE_SPACE_LOCATION};
    if (XR_FAILED(xrLocateSpace(view_, local_, t, &loc))) return false;
    if (!(loc.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT)) return false;
    head = loc.pose;
    return true;
}

}  // namespace xr
