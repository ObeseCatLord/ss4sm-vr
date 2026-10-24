#pragma once
// In-process OpenXR runtime for the game: instance/system, Vulkan enable2 creation, session state machine
// and frame submission. All XR calls that touch the graphics queue run under the queue lock (vk/queue_lock).
#include <vulkan/vulkan.h>
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>
#include <cstdint>
#include <string>
#include <vector>

namespace xr {

struct Swapchain {
    XrSwapchain handle = XR_NULL_HANDLE;
    int64_t format = 0;
    uint32_t width = 0, height = 0, layers = 1;
    std::vector<VkImage> images;
};

struct Views {
    bool valid = false;
    XrTime time = 0;
    XrView inView[2]{};   // eyes relative to the head (VIEW space): IPD and eye rotation
    XrPosef head{};       // VIEW in the play space (STAGE: y = height above the floor)
    XrFovf fov[2]{};
    uint32_t recommendedWidth = 0, recommendedHeight = 0;
};

class Runtime {
public:
    // Creates the XR instance and finds the HMD system. False: no runtime/HMD; caller passes through flat.
    bool initialize();
    // initialize() once per process (cached result); safe to call from any early game hook.
    bool ensure();
    // Instance extensions that were enabled (e.g. XR_EXT_eye_gaze_interaction for eye-tracked foveation).
    bool hasExtension(const char* name) const;
    const std::string& systemName() const { return systemName_; }
    bool eyeGazeSupported() const { return eyeGaze_; }
    bool active() const { return system_ != XR_NULL_SYSTEM_ID; }

    // Called from the hooked vkCreateInstance / vkCreateDevice instead of the real functions.
    VkResult createVulkanInstance(PFN_vkGetInstanceProcAddr gipa, const VkInstanceCreateInfo* ci, VkInstance* out);
    VkResult createVulkanDevice(PFN_vkGetInstanceProcAddr gipa, VkPhysicalDevice pd, const VkDeviceCreateInfo* ci, VkDevice* out);
    VkPhysicalDevice requiredPhysicalDevice(VkInstance inst);

    // Session against the game's graphics queue.
    bool createSession(VkInstance inst, VkPhysicalDevice pd, VkDevice dev, uint32_t queueFamily, uint32_t queueIndex);
    void pollEvents();
    bool running() const { return running_; }

    bool createSwapchain(Swapchain& sc, int64_t preferredFormat, uint32_t w, uint32_t h, uint32_t layers, XrSwapchainUsageFlags usage);
    int64_t pickColorFormat(const int64_t* preferred, size_t n) const;
    bool acquire(Swapchain& sc, uint32_t& index);
    void release(Swapchain& sc);

    // Frame: wait/begin, then end with the given layers.
    bool waitBeginFrame(XrFrameState& fs);
    bool endFrame(const XrFrameState& fs, const XrCompositionLayerBaseHeader* const* layers, uint32_t count);
    bool locateHead(XrTime t, XrPosef& head);
    // Locate head and eyes for the predicted display time of the current frame (call after waitBeginFrame).
    void updateViews(XrTime t);
    const Views& views() const { return views_; }
    XrSpace playSpace() const { return stage_ ? stage_ : local_; }
    XrSpace viewSpace() const { return view_; }

    XrInstance instance() const { return instance_; }
    XrSession session() const { return session_; }
    XrSpace localSpace() const { return local_; }

private:
    XrInstance instance_ = XR_NULL_HANDLE;
    XrSystemId system_ = XR_NULL_SYSTEM_ID;
    XrSession session_ = XR_NULL_HANDLE;
    XrSpace local_ = XR_NULL_HANDLE, view_ = XR_NULL_HANDLE, stage_ = XR_NULL_HANDLE;
    Views views_;
    XrSessionState state_ = XR_SESSION_STATE_UNKNOWN;
    bool running_ = false;
    std::vector<std::string> enabled_;
    std::string systemName_;
    bool eyeGaze_ = false;
    std::vector<int64_t> formats_;
    PFN_xrGetVulkanGraphicsRequirements2KHR getRequirements_ = nullptr;
    PFN_xrCreateVulkanInstanceKHR createInstance_ = nullptr;
    PFN_xrCreateVulkanDeviceKHR createDevice_ = nullptr;
    PFN_xrGetVulkanGraphicsDevice2KHR getDevice_ = nullptr;
};

Runtime& runtime();
const char* resultName(XrResult r);

}  // namespace xr
