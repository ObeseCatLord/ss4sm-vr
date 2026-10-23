// Gate R5 probe: OpenXR + Vulkan (XR_KHR_vulkan_enable2) interop inside the game's Proton prefix.
// Checks runtime-mediated instance/device creation, multiview, fragment shading rate, a 2-layer
// swapchain, the visibility mask and a short frame loop that clears each eye layer.
#include <windows.h>
#include <vulkan/vulkan.h>
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

static FILE* g_log = stdout;
#define LOG(...) do { fprintf(g_log, __VA_ARGS__); fputc('\n', g_log); fflush(g_log); } while (0)

static PFN_vkGetInstanceProcAddr vkGIPA;
#define VKI(inst, name) reinterpret_cast<PFN_##name>(vkGIPA(inst, #name))

static XrInstance xr = XR_NULL_HANDLE;
template <class T> static T xrfn(const char* name) {
    PFN_xrVoidFunction f = nullptr;
    xrGetInstanceProcAddr(xr, name, &f);
    return reinterpret_cast<T>(f);
}

static bool ok(XrResult r, const char* what) {
    if (XR_SUCCEEDED(r)) return true;
    char buf[XR_MAX_RESULT_STRING_SIZE] = {};
    if (xr) xrResultToString(xr, r, buf);
    LOG("FAIL %s: %d %s", what, (int)r, buf);
    return false;
}
static bool vok(VkResult r, const char* what) {
    if (r == VK_SUCCESS) return true;
    LOG("FAIL %s: VkResult %d", what, (int)r);
    return false;
}

int main(int argc, char** argv) {
    if (argc > 1) g_log = fopen(argv[1], "w");
    LOG("vk_xr_probe start");

    HMODULE vkdll = LoadLibraryA("vulkan-1.dll");
    if (!vkdll) { LOG("FAIL LoadLibrary vulkan-1.dll"); return 1; }
    vkGIPA = reinterpret_cast<PFN_vkGetInstanceProcAddr>(GetProcAddress(vkdll, "vkGetInstanceProcAddr"));

    uint32_t n = 0;
    xrEnumerateInstanceExtensionProperties(nullptr, 0, &n, nullptr);
    std::vector<XrExtensionProperties> exts(n, {XR_TYPE_EXTENSION_PROPERTIES});
    xrEnumerateInstanceExtensionProperties(nullptr, n, &n, exts.data());
    auto has = [&](const char* e) {
        for (auto& x : exts) if (!strcmp(x.extensionName, e)) return true;
        return false;
    };
    for (auto& x : exts) LOG("xr ext %s v%u", x.extensionName, x.extensionVersion);
    if (!has(XR_KHR_VULKAN_ENABLE2_EXTENSION_NAME)) { LOG("FAIL no XR_KHR_vulkan_enable2"); return 2; }

    std::vector<const char*> want = {XR_KHR_VULKAN_ENABLE2_EXTENSION_NAME};
    for (const char* e : {XR_KHR_VISIBILITY_MASK_EXTENSION_NAME, XR_KHR_COMPOSITION_LAYER_CYLINDER_EXTENSION_NAME,
                          XR_KHR_COMPOSITION_LAYER_DEPTH_EXTENSION_NAME, XR_EXT_HAND_TRACKING_EXTENSION_NAME,
                          XR_EXT_EYE_GAZE_INTERACTION_EXTENSION_NAME})
        if (has(e)) want.push_back(e);

    XrInstanceCreateInfo ici{XR_TYPE_INSTANCE_CREATE_INFO};
    strcpy(ici.applicationInfo.applicationName, "ss4smvr_vk_xr_probe");
    ici.applicationInfo.apiVersion = XR_API_VERSION_1_0;
    ici.enabledExtensionCount = (uint32_t)want.size();
    ici.enabledExtensionNames = want.data();
    if (!ok(xrCreateInstance(&ici, &xr), "xrCreateInstance")) return 3;
    XrInstanceProperties ip{XR_TYPE_INSTANCE_PROPERTIES};
    xrGetInstanceProperties(xr, &ip);
    LOG("runtime %s %u.%u.%u", ip.runtimeName, XR_VERSION_MAJOR(ip.runtimeVersion),
        XR_VERSION_MINOR(ip.runtimeVersion), XR_VERSION_PATCH(ip.runtimeVersion));

    XrSystemGetInfo sgi{XR_TYPE_SYSTEM_GET_INFO};
    sgi.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
    XrSystemId sys;
    if (!ok(xrGetSystem(xr, &sgi, &sys), "xrGetSystem")) return 4;
    XrSystemProperties sp{XR_TYPE_SYSTEM_PROPERTIES};
    xrGetSystemProperties(xr, sys, &sp);
    LOG("system %s vendor %u maxLayers %u", sp.systemName, sp.vendorId, sp.graphicsProperties.maxLayerCount);

    auto getReq = xrfn<PFN_xrGetVulkanGraphicsRequirements2KHR>("xrGetVulkanGraphicsRequirements2KHR");
    auto createInst = xrfn<PFN_xrCreateVulkanInstanceKHR>("xrCreateVulkanInstanceKHR");
    auto getDev = xrfn<PFN_xrGetVulkanGraphicsDevice2KHR>("xrGetVulkanGraphicsDevice2KHR");
    auto createDev = xrfn<PFN_xrCreateVulkanDeviceKHR>("xrCreateVulkanDeviceKHR");
    XrGraphicsRequirementsVulkan2KHR req{XR_TYPE_GRAPHICS_REQUIREMENTS_VULKAN2_KHR};
    if (!ok(getReq(xr, sys, &req), "xrGetVulkanGraphicsRequirements2KHR")) return 5;
    LOG("vk api min %u.%u max %u.%u", XR_VERSION_MAJOR(req.minApiVersionSupported), XR_VERSION_MINOR(req.minApiVersionSupported),
        XR_VERSION_MAJOR(req.maxApiVersionSupported), XR_VERSION_MINOR(req.maxApiVersionSupported));

    VkApplicationInfo ai{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    ai.pApplicationName = "ss4smvr_vk_xr_probe";
    ai.apiVersion = VK_API_VERSION_1_2;
    VkInstanceCreateInfo vici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    vici.pApplicationInfo = &ai;
    XrVulkanInstanceCreateInfoKHR xvici{XR_TYPE_VULKAN_INSTANCE_CREATE_INFO_KHR};
    xvici.systemId = sys;
    xvici.pfnGetInstanceProcAddr = vkGIPA;
    xvici.vulkanCreateInfo = &vici;
    VkInstance inst; VkResult vr;
    if (!ok(createInst(xr, &xvici, &inst, &vr), "xrCreateVulkanInstanceKHR") || !vok(vr, "vkCreateInstance")) return 6;

    XrVulkanGraphicsDeviceGetInfoKHR gdi{XR_TYPE_VULKAN_GRAPHICS_DEVICE_GET_INFO_KHR};
    gdi.systemId = sys; gdi.vulkanInstance = inst;
    VkPhysicalDevice phys;
    if (!ok(getDev(xr, &gdi, &phys), "xrGetVulkanGraphicsDevice2KHR")) return 7;
    VkPhysicalDeviceProperties pp;
    VKI(inst, vkGetPhysicalDeviceProperties)(phys, &pp);
    LOG("physical device %s api %u.%u.%u", pp.deviceName, VK_VERSION_MAJOR(pp.apiVersion), VK_VERSION_MINOR(pp.apiVersion), VK_VERSION_PATCH(pp.apiVersion));

    uint32_t dn = 0;
    auto enumDevExt = VKI(inst, vkEnumerateDeviceExtensionProperties);
    enumDevExt(phys, nullptr, &dn, nullptr);
    std::vector<VkExtensionProperties> dexts(dn);
    enumDevExt(phys, nullptr, &dn, dexts.data());
    auto dhas = [&](const char* e) { for (auto& x : dexts) if (!strcmp(x.extensionName, e)) return true; return false; };
    bool fsr = dhas(VK_KHR_FRAGMENT_SHADING_RATE_EXTENSION_NAME);
    LOG("dev ext fragment_shading_rate=%d multiview=%d create_renderpass2=%d nv_stereo=%d",
        fsr, dhas(VK_KHR_MULTIVIEW_EXTENSION_NAME), dhas(VK_KHR_CREATE_RENDERPASS_2_EXTENSION_NAME),
        dhas("VK_NV_viewport_array2"));

    VkPhysicalDeviceMultiviewFeatures mvq{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MULTIVIEW_FEATURES};
    VkPhysicalDeviceFeatures2 f2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
    f2.pNext = &mvq;
    VKI(inst, vkGetPhysicalDeviceFeatures2)(phys, &f2);
    LOG("multiview feature=%u geometryShader=%u tessellation=%u", mvq.multiview, mvq.multiviewGeometryShader, mvq.multiviewTessellationShader);

    uint32_t qn = 0;
    auto qprops = VKI(inst, vkGetPhysicalDeviceQueueFamilyProperties);
    qprops(phys, &qn, nullptr);
    std::vector<VkQueueFamilyProperties> qf(qn);
    qprops(phys, &qn, qf.data());
    uint32_t qfi = 0;
    for (uint32_t i = 0; i < qn; ++i) if (qf[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) { qfi = i; break; }

    float prio = 1.0f;
    VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    qci.queueFamilyIndex = qfi; qci.queueCount = 1; qci.pQueuePriorities = &prio;
    VkPhysicalDeviceMultiviewFeatures mv{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MULTIVIEW_FEATURES};
    mv.multiview = VK_TRUE;
    std::vector<const char*> devExts;
    if (fsr) devExts.push_back(VK_KHR_FRAGMENT_SHADING_RATE_EXTENSION_NAME);
    VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    dci.pNext = &mv;
    dci.queueCreateInfoCount = 1; dci.pQueueCreateInfos = &qci;
    dci.enabledExtensionCount = (uint32_t)devExts.size(); dci.ppEnabledExtensionNames = devExts.data();
    XrVulkanDeviceCreateInfoKHR xdci{XR_TYPE_VULKAN_DEVICE_CREATE_INFO_KHR};
    xdci.systemId = sys; xdci.pfnGetInstanceProcAddr = vkGIPA; xdci.vulkanPhysicalDevice = phys; xdci.vulkanCreateInfo = &dci;
    VkDevice dev;
    if (!ok(createDev(xr, &xdci, &dev, &vr), "xrCreateVulkanDeviceKHR") || !vok(vr, "vkCreateDevice")) return 8;
    LOG("device created (multiview on, fsr %d)", fsr);

    auto gdpa = VKI(inst, vkGetDeviceProcAddr);
#define VKD(name) reinterpret_cast<PFN_##name>(gdpa(dev, #name))
    VkQueue queue;
    VKD(vkGetDeviceQueue)(dev, qfi, 0, &queue);

    XrGraphicsBindingVulkan2KHR gb{XR_TYPE_GRAPHICS_BINDING_VULKAN2_KHR};
    gb.instance = inst; gb.physicalDevice = phys; gb.device = dev; gb.queueFamilyIndex = qfi; gb.queueIndex = 0;
    XrSessionCreateInfo sci{XR_TYPE_SESSION_CREATE_INFO};
    sci.next = &gb; sci.systemId = sys;
    XrSession session;
    if (!ok(xrCreateSession(xr, &sci, &session), "xrCreateSession")) return 9;
    LOG("session created");

    uint32_t vn = 0;
    xrEnumerateViewConfigurationViews(xr, sys, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 0, &vn, nullptr);
    std::vector<XrViewConfigurationView> views(vn, {XR_TYPE_VIEW_CONFIGURATION_VIEW});
    xrEnumerateViewConfigurationViews(xr, sys, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, vn, &vn, views.data());
    for (auto& v : views) LOG("view rec %ux%u max %ux%u samples %u", v.recommendedImageRectWidth, v.recommendedImageRectHeight,
                              v.maxImageRectWidth, v.maxImageRectHeight, v.recommendedSwapchainSampleCount);

    uint32_t fn = 0;
    xrEnumerateSwapchainFormats(session, 0, &fn, nullptr);
    std::vector<int64_t> fmts(fn);
    xrEnumerateSwapchainFormats(session, fn, &fn, fmts.data());
    int64_t fmt = fmts.empty() ? 0 : fmts[0];
    for (auto f : fmts) if (f == VK_FORMAT_R8G8B8A8_SRGB || f == VK_FORMAT_B8G8R8A8_SRGB) { fmt = f; break; }
    std::string fl; for (auto f : fmts) fl += std::to_string(f) + " ";
    LOG("swapchain formats: %s chosen %lld", fl.c_str(), (long long)fmt);

    XrSwapchainCreateInfo swci{XR_TYPE_SWAPCHAIN_CREATE_INFO};
    swci.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT;
    swci.format = fmt; swci.sampleCount = 1;
    swci.width = views[0].recommendedImageRectWidth; swci.height = views[0].recommendedImageRectHeight;
    swci.faceCount = 1; swci.arraySize = 2; swci.mipCount = 1;
    XrSwapchain swap;
    if (!ok(xrCreateSwapchain(session, &swci, &swap), "xrCreateSwapchain arraySize=2")) return 10;
    uint32_t in = 0;
    xrEnumerateSwapchainImages(swap, 0, &in, nullptr);
    std::vector<XrSwapchainImageVulkan2KHR> imgs(in, {XR_TYPE_SWAPCHAIN_IMAGE_VULKAN2_KHR});
    xrEnumerateSwapchainImages(swap, in, &in, reinterpret_cast<XrSwapchainImageBaseHeader*>(imgs.data()));
    LOG("swapchain %ux%u x2 layers, %u images", swci.width, swci.height, in);

    if (has(XR_KHR_VISIBILITY_MASK_EXTENSION_NAME)) {
        auto getMask = xrfn<PFN_xrGetVisibilityMaskKHR>("xrGetVisibilityMaskKHR");
        for (uint32_t eye = 0; eye < 2; ++eye) {
            XrVisibilityMaskKHR m{XR_TYPE_VISIBILITY_MASK_KHR};
            XrResult r = getMask(session, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, eye, XR_VISIBILITY_MASK_TYPE_HIDDEN_TRIANGLE_MESH_KHR, &m);
            LOG("visibility mask eye %u: r=%d vertices=%u indices=%u", eye, (int)r, m.vertexCountOutput, m.indexCountOutput);
        }
    }

    VkCommandPoolCreateInfo cpci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    cpci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT; cpci.queueFamilyIndex = qfi;
    VkCommandPool pool; VKD(vkCreateCommandPool)(dev, &cpci, nullptr, &pool);
    VkCommandBufferAllocateInfo cbai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    cbai.commandPool = pool; cbai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; cbai.commandBufferCount = 1;
    VkCommandBuffer cb; VKD(vkAllocateCommandBuffers)(dev, &cbai, &cb);
    VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    VkFence fence; VKD(vkCreateFence)(dev, &fci, nullptr, &fence);

    XrReferenceSpaceCreateInfo rsci{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
    rsci.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
    rsci.poseInReferenceSpace.orientation.w = 1;
    XrSpace space; xrCreateReferenceSpace(session, &rsci, &space);

    bool running = false; int frames = 0; DWORD start = GetTickCount();
    double waitMs = 0;
    while (frames < 300 && GetTickCount() - start < 30000) {
        XrEventDataBuffer ev{XR_TYPE_EVENT_DATA_BUFFER};
        while (xrPollEvent(xr, &ev) == XR_SUCCESS) {
            if (ev.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED) {
                auto* s = reinterpret_cast<XrEventDataSessionStateChanged*>(&ev);
                LOG("session state %d", (int)s->state);
                if (s->state == XR_SESSION_STATE_READY) {
                    XrSessionBeginInfo bi{XR_TYPE_SESSION_BEGIN_INFO};
                    bi.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
                    running = ok(xrBeginSession(session, &bi), "xrBeginSession");
                }
                if (s->state == XR_SESSION_STATE_STOPPING || s->state == XR_SESSION_STATE_EXITING) frames = 1 << 20;
            }
            ev = {XR_TYPE_EVENT_DATA_BUFFER};
        }
        if (!running) { Sleep(10); continue; }
        LARGE_INTEGER t0, t1, fq; QueryPerformanceCounter(&t0); QueryPerformanceFrequency(&fq);
        XrFrameState fs{XR_TYPE_FRAME_STATE};
        if (!ok(xrWaitFrame(session, nullptr, &fs), "xrWaitFrame")) break;
        QueryPerformanceCounter(&t1); waitMs += 1000.0 * (t1.QuadPart - t0.QuadPart) / fq.QuadPart;
        xrBeginFrame(session, nullptr);

        XrViewLocateInfo vli{XR_TYPE_VIEW_LOCATE_INFO};
        vli.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO; vli.displayTime = fs.predictedDisplayTime; vli.space = space;
        XrViewState vs{XR_TYPE_VIEW_STATE}; XrView xv[2] = {{XR_TYPE_VIEW}, {XR_TYPE_VIEW}}; uint32_t vc = 0;
        xrLocateViews(session, &vli, &vs, 2, &vc, xv);

        uint32_t idx; xrAcquireSwapchainImage(swap, nullptr, &idx);
        XrSwapchainImageWaitInfo wi{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO}; wi.timeout = XR_INFINITE_DURATION;
        xrWaitSwapchainImage(swap, &wi);
        VkImage image = imgs[idx].image;
        VkCommandBufferBeginInfo cbbi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        VKD(vkResetCommandBuffer)(cb, 0); VKD(vkBeginCommandBuffer)(cb, &cbbi);
        VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED; b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED; b.image = image;
        b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 2}; b.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        VKD(vkCmdPipelineBarrier)(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);
        for (uint32_t eye = 0; eye < 2; ++eye) {
            VkClearColorValue c{}; c.float32[0] = eye ? 0.0f : 0.6f; c.float32[2] = eye ? 0.6f : 0.0f; c.float32[3] = 1;
            VkImageSubresourceRange r{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, eye, 1};
            VKD(vkCmdClearColorImage)(cb, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &c, 1, &r);
        }
        b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL; b.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT; b.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        VKD(vkCmdPipelineBarrier)(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);
        VKD(vkEndCommandBuffer)(cb);
        VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO}; si.commandBufferCount = 1; si.pCommandBuffers = &cb;
        VKD(vkQueueSubmit)(queue, 1, &si, fence);
        VKD(vkWaitForFences)(dev, 1, &fence, VK_TRUE, UINT64_MAX); VKD(vkResetFences)(dev, 1, &fence);
        xrReleaseSwapchainImage(swap, nullptr);

        XrCompositionLayerProjectionView pv[2] = {{XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW}, {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW}};
        for (uint32_t eye = 0; eye < 2; ++eye) {
            pv[eye].pose = xv[eye].pose; pv[eye].fov = xv[eye].fov;
            pv[eye].subImage.swapchain = swap; pv[eye].subImage.imageArrayIndex = eye;
            pv[eye].subImage.imageRect = {{0, 0}, {(int32_t)swci.width, (int32_t)swci.height}};
        }
        XrCompositionLayerProjection proj{XR_TYPE_COMPOSITION_LAYER_PROJECTION};
        proj.space = space; proj.viewCount = 2; proj.views = pv;
        const XrCompositionLayerBaseHeader* layers[] = {reinterpret_cast<XrCompositionLayerBaseHeader*>(&proj)};
        XrFrameEndInfo fei{XR_TYPE_FRAME_END_INFO};
        fei.displayTime = fs.predictedDisplayTime; fei.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
        fei.layerCount = fs.shouldRender ? 1 : 0; fei.layers = layers;
        if (!ok(xrEndFrame(session, &fei), "xrEndFrame")) break;
        if (++frames == 1) LOG("first frame submitted, period %lld ns, eye0 fov L%.3f R%.3f", (long long)fs.predictedDisplayPeriod,
                               xv[0].fov.angleLeft, xv[0].fov.angleRight);
    }
    LOG("frames %d avg xrWaitFrame %.2f ms", frames, frames ? waitMs / frames : 0.0);
    if (running) xrRequestExitSession(session);
    xrDestroySwapchain(swap);
    xrDestroySession(session);
    xrDestroyInstance(xr);
    LOG("vk_xr_probe %s", frames >= 100 ? "PASS" : "INCOMPLETE");
    return frames >= 100 ? 0 : 11;
}
