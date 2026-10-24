#include "up/upscaler.hpp"
#include "se4/log.hpp"
#include "se4/options.hpp"
#include "up/ngx_shim.hpp"
#include <ffx_api/ffx_api.h>
#include <ffx_api/ffx_api_loader.h>
#include <ffx_api/ffx_upscale.h>
#include <ffx_api/vk/ffx_api_vk.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>

namespace up {
namespace {
Kind g_kind = Kind::Off;
int g_quality = 0;       // 0 native AA, 1 quality, 2 balanced, 3 performance
float g_sharpness = 0.15f;
VkInstance g_inst;
VkPhysicalDevice g_pd;
VkDevice g_dev;
PFN_vkGetDeviceProcAddr g_gdpa;
bool g_reset[2] = {true, true};

const float kRatio[4] = {1.0f, 1.5f, 1.7f, 2.0f};

float halton(int index, int base) {
    float f = 1, r = 0;
    for (int i = index; i > 0; i /= base) {
        f /= float(base);
        r += f * float(i % base);
    }
    return r;
}

// ---------------- DLSS (NGX Vulkan) ----------------
struct Dlss {
    bool ready = false;
    NVSDK_NGX_Parameter* params = nullptr;
    NVSDK_NGX_Handle* feature[2] = {};
    uint32_t rw[2] = {}, rh[2] = {}, ow[2] = {}, oh[2] = {};
    int flags[2] = {};
} g_dlss;

NVSDK_NGX_PerfQuality_Value dlssQuality() {
    switch (g_quality) {
    case 0: return NVSDK_NGX_PerfQuality_Value_DLAA;
    case 1: return NVSDK_NGX_PerfQuality_Value_MaxQuality;
    case 2: return NVSDK_NGX_PerfQuality_Value_Balanced;
    default: return NVSDK_NGX_PerfQuality_Value_MaxPerf;
    }
}

bool dlssInit(PFN_vkGetInstanceProcAddr gipa, const wchar_t* dir) {
    if (!ngx::load()) return false;
    auto init = reinterpret_cast<ngx::InitExt2Fn>(ngx::sym("NVSDK_NGX_VULKAN_Init_Ext2"));
    if (!init) {
        VRLOG("dlss: NGX core lacks NVSDK_NGX_VULKAN_Init_Ext2");
        return false;
    }
    // Application id: custom engine integration; NGX logs go to the mod directory.
    NVSDK_NGX_Result r = init(0x5353344dull /* "SS4M" */, dir, g_inst, g_pd, g_dev, gipa, g_gdpa, NVSDK_NGX_Version_API, nullptr);
    if (NVSDK_NGX_FAILED(r)) {
        VRLOG("dlss: NGX init failed (0x%x)", unsigned(r));
        return false;
    }
    if (NVSDK_NGX_FAILED(NVSDK_NGX_VULKAN_GetCapabilityParameters(&g_dlss.params)) || !g_dlss.params) {
        VRLOG("dlss: no capability parameters");
        return false;
    }
    if (!ngx::selfTest(g_dlss.params)) return false;
    int available = 0;
    NVSDK_NGX_Parameter_GetI(g_dlss.params, NVSDK_NGX_Parameter_SuperSampling_Available, &available);
    if (!available) {
        int needsUpdate = 0;
        NVSDK_NGX_Parameter_GetI(g_dlss.params, NVSDK_NGX_Parameter_SuperSampling_NeedsUpdatedDriver, &needsUpdate);
        VRLOG("dlss: super sampling not available%s", needsUpdate ? " (driver update needed)" : "");
        return false;
    }
    g_dlss.ready = true;
    return true;
}

bool dlssEvaluate(int eye, const Frame& f) {
    const int e = eye - 1;
    int flags = NVSDK_NGX_DLSS_Feature_Flags_MVLowRes | NVSDK_NGX_DLSS_Feature_Flags_AutoExposure;
    if (f.hdr) flags |= NVSDK_NGX_DLSS_Feature_Flags_IsHDR;
    if (f.depthInverted) flags |= NVSDK_NGX_DLSS_Feature_Flags_DepthInverted;
    const bool resize = g_dlss.rw[e] != f.color.width || g_dlss.rh[e] != f.color.height || g_dlss.ow[e] != f.output.width ||
                        g_dlss.oh[e] != f.output.height || g_dlss.flags[e] != flags;
    if (!g_dlss.feature[e] || resize) {
        if (g_dlss.feature[e]) NVSDK_NGX_VULKAN_ReleaseFeature(g_dlss.feature[e]), g_dlss.feature[e] = nullptr;
        // Transformer model (preset K) for every mode: best temporal stability, which matters most in VR.
        for (const char* p : {NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_DLAA, NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Quality,
                              NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Balanced, NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Performance})
            NVSDK_NGX_Parameter_SetUI(g_dlss.params, p, NVSDK_NGX_DLSS_Hint_Render_Preset_K);
        NVSDK_NGX_DLSS_Create_Params cp{};
        cp.Feature.InWidth = f.color.width;
        cp.Feature.InHeight = f.color.height;
        cp.Feature.InTargetWidth = f.output.width;
        cp.Feature.InTargetHeight = f.output.height;
        cp.Feature.InPerfQualityValue = dlssQuality();
        cp.InFeatureCreateFlags = flags;
        NVSDK_NGX_Result r = NGX_VULKAN_CREATE_DLSS_EXT1(g_dev, f.cmd, 1, 1, &g_dlss.feature[e], g_dlss.params, &cp);
        if (NVSDK_NGX_FAILED(r)) {
            VRLOG("dlss: eye %d feature creation failed (0x%x)", eye, unsigned(r));
            g_dlss.feature[e] = nullptr;
            return false;
        }
        g_dlss.rw[e] = f.color.width, g_dlss.rh[e] = f.color.height, g_dlss.ow[e] = f.output.width, g_dlss.oh[e] = f.output.height;
        g_dlss.flags[e] = flags;
        g_reset[e] = true;
        VRLOG("dlss: eye %d %ux%u -> %ux%u (quality %d, preset K)", eye, f.color.width, f.color.height, f.output.width, f.output.height, g_quality);
    }
    auto res = [](const Image& i, bool rw, VkImageAspectFlags aspect) {
        return NVSDK_NGX_Create_ImageView_Resource_VK(i.view, i.image, {aspect, 0, 1, 0, 1}, i.format, i.width, i.height, rw);
    };
    NVSDK_NGX_Resource_VK color = res(f.color, false, VK_IMAGE_ASPECT_COLOR_BIT);
    NVSDK_NGX_Resource_VK out = res(f.output, true, VK_IMAGE_ASPECT_COLOR_BIT);
    NVSDK_NGX_Resource_VK depth = res(f.depth, false, VK_IMAGE_ASPECT_DEPTH_BIT);
    NVSDK_NGX_Resource_VK mv = res(f.motion, false, VK_IMAGE_ASPECT_COLOR_BIT);
    NVSDK_NGX_VK_DLSS_Eval_Params ep{};
    ep.Feature.pInColor = &color;
    ep.Feature.pInOutput = &out;
    ep.pInDepth = &depth;
    ep.pInMotionVectors = &mv;
    ep.InJitterOffsetX = f.jitterX;
    ep.InJitterOffsetY = f.jitterY;
    ep.InRenderSubrectDimensions = {f.color.width, f.color.height};
    ep.InReset = (g_reset[e] || f.reset) ? 1 : 0;
    ep.InMVScaleX = f.motionScaleX;
    ep.InMVScaleY = f.motionScaleY;
    ep.InFrameTimeDeltaInMsec = f.frameMs;
    NVSDK_NGX_Result r = NGX_VULKAN_EVALUATE_DLSS_EXT(f.cmd, g_dlss.feature[e], g_dlss.params, &ep);
    g_reset[e] = false;
    if (NVSDK_NGX_FAILED(r)) {
        static int logged;
        if (logged++ < 4) VRLOG("dlss: eye %d evaluate failed (0x%x)", eye, unsigned(r));
        return false;
    }
    return true;
}

// ---------------- FSR 3.1 (FidelityFX API, Vulkan) ----------------
struct Fsr {
    HMODULE dll = nullptr;
    ffxFunctions fn{};
    ffxContext ctx[2] = {};
    uint32_t maxRender[2][2] = {}, maxOut[2][2] = {};
    uint32_t flags[2] = {};
} g_fsr;

bool fsrInit(const wchar_t* dir) {
    std::wstring path = std::wstring(dir) + L"\\amd_fidelityfx_vk.dll";
    g_fsr.dll = LoadLibraryW(path.c_str());
    if (!g_fsr.dll) {
        VRLOG("fsr: amd_fidelityfx_vk.dll not found next to the mod");
        return false;
    }
    ffxLoadFunctions(&g_fsr.fn, g_fsr.dll);
    if (!g_fsr.fn.CreateContext || !g_fsr.fn.Dispatch || !g_fsr.fn.DestroyContext) {
        VRLOG("fsr: FidelityFX API entry points missing");
        return false;
    }
    return true;
}

FfxApiResource fsrRes(const Image& i, uint32_t state, uint32_t usage) {
    VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ci.imageType = VK_IMAGE_TYPE_2D;
    ci.format = i.format;
    ci.extent = {i.width, i.height, 1};
    ci.mipLevels = 1;
    ci.arrayLayers = 1;
    ci.samples = VK_SAMPLE_COUNT_1_BIT;
    return ffxApiGetResourceVK(reinterpret_cast<void*>(i.image), ffxApiGetImageResourceDescriptionVK(i.image, ci, usage), state);
}

bool fsrEvaluate(int eye, const Frame& f) {
    const int e = eye - 1;
    uint32_t flags = FFX_UPSCALE_ENABLE_AUTO_EXPOSURE;
    if (f.hdr) flags |= FFX_UPSCALE_ENABLE_HIGH_DYNAMIC_RANGE;
    if (f.depthInverted) flags |= FFX_UPSCALE_ENABLE_DEPTH_INVERTED;
    if (!g_fsr.ctx[e] || g_fsr.maxRender[e][0] < f.color.width || g_fsr.maxRender[e][1] < f.color.height ||
        g_fsr.maxOut[e][0] != f.output.width || g_fsr.maxOut[e][1] != f.output.height || g_fsr.flags[e] != flags) {
        if (g_fsr.ctx[e]) g_fsr.fn.DestroyContext(&g_fsr.ctx[e], nullptr), g_fsr.ctx[e] = nullptr;
        ffxCreateBackendVKDesc be{};
        be.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_VK;
        be.vkDevice = g_dev;
        be.vkPhysicalDevice = g_pd;
        be.vkDeviceProcAddr = g_gdpa;
        ffxCreateContextDescUpscale cd{};
        cd.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE;
        cd.header.pNext = &be.header;
        cd.flags = flags;
        cd.maxRenderSize = {f.color.width, f.color.height};
        cd.maxUpscaleSize = {f.output.width, f.output.height};
        ffxReturnCode_t rc = g_fsr.fn.CreateContext(&g_fsr.ctx[e], &cd.header, nullptr);
        if (rc != FFX_API_RETURN_OK) {
            VRLOG("fsr: eye %d context creation failed (%u)", eye, unsigned(rc));
            g_fsr.ctx[e] = nullptr;
            return false;
        }
        g_fsr.maxRender[e][0] = f.color.width, g_fsr.maxRender[e][1] = f.color.height;
        g_fsr.maxOut[e][0] = f.output.width, g_fsr.maxOut[e][1] = f.output.height;
        g_fsr.flags[e] = flags;
        g_reset[e] = true;
        VRLOG("fsr: eye %d %ux%u -> %ux%u (quality %d)", eye, f.color.width, f.color.height, f.output.width, f.output.height, g_quality);
    }
    ffxDispatchDescUpscale d{};
    d.header.type = FFX_API_DISPATCH_DESC_TYPE_UPSCALE;
    d.commandList = f.cmd;
    d.color = fsrRes(f.color, FFX_API_RESOURCE_STATE_COMPUTE_READ, FFX_API_RESOURCE_USAGE_READ_ONLY);
    d.depth = fsrRes(f.depth, FFX_API_RESOURCE_STATE_COMPUTE_READ, FFX_API_RESOURCE_USAGE_DEPTHTARGET);
    d.motionVectors = fsrRes(f.motion, FFX_API_RESOURCE_STATE_COMPUTE_READ, FFX_API_RESOURCE_USAGE_READ_ONLY);
    d.output = fsrRes(f.output, FFX_API_RESOURCE_STATE_UNORDERED_ACCESS, FFX_API_RESOURCE_USAGE_UAV);
    d.jitterOffset = {f.jitterX, f.jitterY};
    d.motionVectorScale = {f.motionScaleX, f.motionScaleY};
    d.renderSize = {f.color.width, f.color.height};
    d.upscaleSize = {f.output.width, f.output.height};
    d.enableSharpening = g_sharpness > 0.0f;
    d.sharpness = g_sharpness;
    d.frameTimeDelta = f.frameMs;
    d.preExposure = 1.0f;
    d.reset = g_reset[e] || f.reset;
    d.cameraNear = f.depthInverted ? f.farZ : f.nearZ;
    d.cameraFar = f.depthInverted ? f.nearZ : f.farZ;
    d.cameraFovAngleVertical = f.fovY;
    d.viewSpaceToMetersFactor = 1.0f;
    ffxReturnCode_t rc = g_fsr.fn.Dispatch(&g_fsr.ctx[e], &d.header);
    g_reset[e] = false;
    if (rc != FFX_API_RETURN_OK) {
        static int logged;
        if (logged++ < 4) VRLOG("fsr: eye %d dispatch failed (%u)", eye, unsigned(rc));
        return false;
    }
    return true;
}

bool isNvidia(VkInstance inst, PFN_vkGetInstanceProcAddr gipa, VkPhysicalDevice pd) {
    auto gp = reinterpret_cast<PFN_vkGetPhysicalDeviceProperties>(gipa(inst, "vkGetPhysicalDeviceProperties"));
    VkPhysicalDeviceProperties p{};
    if (gp) gp(pd, &p);
    return p.vendorID == 0x10DE;
}
}  // namespace

void instanceExtensions(const char** names, uint32_t& count, uint32_t max) {
    if (se4::options::get("upscaler", -1) == 0 || !ngx::load()) return;
    auto req = reinterpret_cast<decltype(&NVSDK_NGX_VULKAN_RequiredExtensions)>(ngx::sym("NVSDK_NGX_VULKAN_RequiredExtensions"));
    unsigned int ic = 0, dc = 0;
    const char **ie = nullptr, **de = nullptr;
    if (!req || NVSDK_NGX_FAILED(req(&ic, &ie, &dc, &de))) return;
    for (unsigned i = 0; i < ic && count < max; ++i) names[count++] = ie[i];
}

void deviceExtensions(VkInstance, VkPhysicalDevice, const char** names, uint32_t& count, uint32_t max) {
    if (se4::options::get("upscaler", -1) == 0 || !ngx::load()) return;
    auto req = reinterpret_cast<decltype(&NVSDK_NGX_VULKAN_RequiredExtensions)>(ngx::sym("NVSDK_NGX_VULKAN_RequiredExtensions"));
    unsigned int ic = 0, dc = 0;
    const char **ie = nullptr, **de = nullptr;
    if (!req || NVSDK_NGX_FAILED(req(&ic, &ie, &dc, &de))) return;
    for (unsigned i = 0; i < dc && count < max; ++i) names[count++] = de[i];
}

bool init(VkInstance inst, VkPhysicalDevice pd, VkDevice dev, PFN_vkGetInstanceProcAddr gipa, PFN_vkGetDeviceProcAddr gdpa, VkQueue,
          uint32_t, const wchar_t* dir) {
    g_inst = inst, g_pd = pd, g_dev = dev, g_gdpa = gdpa;
    int opt = se4::options::get("upscaler", 0);  // off until validated in the lab; auto = -1
    g_quality = std::clamp(se4::options::get("upscale_quality", 0), 0, 3);
    g_sharpness = std::clamp(se4::options::get("upscale_sharpness", 15), 0, 100) / 100.0f;
    if (opt == 0) {
        VRLOG("upscaler: off");
        return false;
    }
    const bool nvidia = isNvidia(inst, gipa, pd);
    if ((opt == 2 || (opt < 0 && nvidia)) && dlssInit(gipa, dir)) g_kind = Kind::DLSS;
    else if (opt != 2 && fsrInit(dir)) g_kind = Kind::FSR;
    else if (opt == 2 && fsrInit(dir)) g_kind = Kind::FSR;  // DLSS requested but unavailable
    VRLOG("upscaler: %s (quality %d, sharpness %.2f)", name(), g_quality, g_sharpness);
    return g_kind != Kind::Off;
}

Kind active() { return g_kind; }
const char* name() { return g_kind == Kind::DLSS ? "DLSS" : g_kind == Kind::FSR ? "FSR 3.1" : "off"; }

void renderSize(uint32_t outW, uint32_t outH, uint32_t& rw, uint32_t& rh) {
    float r = g_kind == Kind::Off ? 1.0f : kRatio[g_quality];
    rw = std::max(1u, uint32_t(std::lround(outW / r)));
    rh = std::max(1u, uint32_t(std::lround(outH / r)));
}

void jitter(uint32_t frame, uint32_t renderW, uint32_t outW, float& px, float& py) {
    if (g_kind == Kind::Off || !renderW) {
        px = py = 0;
        return;
    }
    // FSR's phase count rule (also fine for DLSS): 8 * (display/render)^2.
    float ratio = float(outW) / float(renderW);
    int phases = std::max(8, int(std::ceil(8.0f * ratio * ratio)));
    int idx = int(frame % uint32_t(phases)) + 1;
    px = halton(idx, 2) - 0.5f;
    py = halton(idx, 3) - 0.5f;
}

float mipBias(uint32_t renderW, uint32_t outW) {
    if (g_kind == Kind::Off || !renderW || !outW) return 0.0f;
    return std::log2(float(renderW) / float(outW));
}

bool evaluate(int eye, const Frame& f) {
    if (eye != 1 && eye != 2) return false;
    if (g_kind == Kind::DLSS) return dlssEvaluate(eye, f);
    if (g_kind == Kind::FSR) return fsrEvaluate(eye, f);
    return false;
}

void resetHistory() { g_reset[0] = g_reset[1] = true; }
}  // namespace up
