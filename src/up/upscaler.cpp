#include "up/upscaler.hpp"
#include "core/upscale.hpp"
#include "se4/build.hpp"
#include "se4/log.hpp"
#include "se4/options.hpp"
#include "up/ngx_shim.hpp"
#include "vk/queue_lock.hpp"
#include <ffx_api/ffx_api.h>
#include <ffx_api/ffx_api_loader.h>
#include <ffx_api/ffx_upscale.h>
#include <ffx_api/vk/ffx_api_vk.h>
#include <algorithm>
#include <atomic>
#include <cfloat>
#include <cmath>
#include <cstring>
#include <string>

namespace up {
namespace {
// The active backend. Desktop menu mode switches it at runtime (select, on the render thread at a frame boundary).
std::atomic<Kind> g_kind{Kind::Off};
int g_quality = 0;       // 0 native AA, 1 quality, 2 balanced, 3 performance
int g_menuStart = -2;    // desktop menu mode: the startup choice (option upscaler encoding); -2 = not set
bool g_sharpnessSet;     // the menu's saved value replaced option upscale_sharpness
std::atomic<float> g_sharpness{0.25f};  // the mod's RCAS pass after either backend (src/up/integrate.cpp)
// Lab switch upscale_fsr_depth: the depth range given to FSR for the reverse-Z depth. 0 swapped (near = far plane),
// 1 as is, 2 infinite (FFX_UPSCALE_ENABLE_DEPTH_INFINITE, near = FLT_MAX, far = the near plane, as AMD's sample does).
int g_fsrDepth;
VkInstance g_inst;
VkPhysicalDevice g_pd;
VkDevice g_dev;
PFN_vkGetDeviceProcAddr g_gdpa;
VkQueue g_queue;
// Per view slot: 0 the flat (desktop) main view, 1 and 2 the VR eyes (src/up/integrate.cpp).
constexpr int kSlots = 3;
const char* slotName(int slot) { return slot == 0 ? "flat" : slot == 1 ? "eye 1" : "eye 2"; }
bool g_reset[kSlots] = {true, true, true};

// ---------------- DLSS (NGX Vulkan) ----------------
struct Dlss {
    bool ready = false;
    NVSDK_NGX_Parameter* params = nullptr;
    NVSDK_NGX_Handle* feature[kSlots] = {};
    uint32_t rw[kSlots] = {}, rh[kSlots] = {}, ow[kSlots] = {}, oh[kSlots] = {};
    int flags[kSlots] = {};
} g_dlss;

// The configured quality mode; in desktop menu mode the one matching the actual ratio (the engine's MPix limit chooses
// the render size there).
NVSDK_NGX_PerfQuality_Value dlssQuality(uint32_t inW, uint32_t outW) {
    switch (menuMode() ? core::upscaleQualityOfRatio(inW, outW) : g_quality) {
    case 0: return NVSDK_NGX_PerfQuality_Value_DLAA;
    case 1: return NVSDK_NGX_PerfQuality_Value_MaxQuality;
    case 2: return NVSDK_NGX_PerfQuality_Value_Balanced;
    case 3: return NVSDK_NGX_PerfQuality_Value_MaxPerf;
    default: return NVSDK_NGX_PerfQuality_Value_UltraPerformance;
    }
}

bool dlssInit(PFN_vkGetInstanceProcAddr gipa, const wchar_t* dir) {
    if (!ngx::load()) return false;
    // Custom-engine project id (no NVIDIA-issued application id). Feature DLLs (nvngx_dlss.dll) are searched in
    // the game's Bin/x64 (the application folder) and the mod folder.
    auto init = reinterpret_cast<ngx::InitProjectIdExtFn>(ngx::sym("NVSDK_NGX_VULKAN_Init_ProjectID_Ext"));
    if (!init) {
        VRLOG("dlss: NGX core lacks NVSDK_NGX_VULKAN_Init_ProjectID_Ext");
        return false;
    }
    static wchar_t exeDir[MAX_PATH];
    GetModuleFileNameW(nullptr, exeDir, MAX_PATH);
    if (wchar_t* slash = wcsrchr(exeDir, L'\\')) *slash = 0;
    static const wchar_t* paths[2];
    paths[0] = exeDir;
    paths[1] = dir;
    NVSDK_NGX_FeatureCommonInfo info{};
    info.PathListInfo.Path = paths;
    info.PathListInfo.Length = 2;
    NVSDK_NGX_Result r = init("6c1f2a3e-5b7d-4e8a-9c0b-5d3f4a2b1c00", NVSDK_NGX_ENGINE_TYPE_CUSTOM, "1.0", dir, g_inst, g_pd, g_dev,
                              gipa, g_gdpa, NVSDK_NGX_Version_API, &info);
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
        int needsUpdate = 0, initResult = 0;
        unsigned int minMajor = 0, minMinor = 0;
        NVSDK_NGX_Parameter_GetI(g_dlss.params, NVSDK_NGX_Parameter_SuperSampling_NeedsUpdatedDriver, &needsUpdate);
        NVSDK_NGX_Parameter_GetI(g_dlss.params, NVSDK_NGX_Parameter_SuperSampling_FeatureInitResult, &initResult);
        NVSDK_NGX_Parameter_GetUI(g_dlss.params, NVSDK_NGX_Parameter_SuperSampling_MinDriverVersionMajor, &minMajor);
        NVSDK_NGX_Parameter_GetUI(g_dlss.params, NVSDK_NGX_Parameter_SuperSampling_MinDriverVersionMinor, &minMinor);
        VRLOG("dlss: super sampling not available (feature init result 0x%x, needs driver update %d, min driver %u.%u)",
              unsigned(initResult), needsUpdate, minMajor, minMinor);
        return false;
    }
    g_dlss.ready = true;
    return true;
}

bool dlssEvaluate(int slot, const Frame& f) {
    const int e = slot;
    const uint32_t inW = f.renderW ? f.renderW : f.color.width, inH = f.renderH ? f.renderH : f.color.height;
    int flags = NVSDK_NGX_DLSS_Feature_Flags_MVLowRes | NVSDK_NGX_DLSS_Feature_Flags_AutoExposure;
    if (f.hdr) flags |= NVSDK_NGX_DLSS_Feature_Flags_IsHDR;
    if (f.depthInverted) flags |= NVSDK_NGX_DLSS_Feature_Flags_DepthInverted;
    const bool resize = g_dlss.rw[e] != inW || g_dlss.rh[e] != inH || g_dlss.ow[e] != f.output.width ||
                        g_dlss.oh[e] != f.output.height || g_dlss.flags[e] != flags;
    if (!g_dlss.feature[e] || resize) {
        if (g_dlss.feature[e]) waitGpuIdle(), NVSDK_NGX_VULKAN_ReleaseFeature(g_dlss.feature[e]), g_dlss.feature[e] = nullptr;
        // Transformer model (preset K) for every mode: best temporal stability, which matters most in VR. Lab option
        // dlss_preset (the SDK's enum value: 5 E, 6 F, 10 J, 11 K, 12 L, 13 M) compares others.
        const unsigned preset = unsigned(std::clamp(se4::options::get("dlss_preset", NVSDK_NGX_DLSS_Hint_Render_Preset_K), 0, 15));
        for (const char* p : {NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_DLAA, NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Quality,
                              NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Balanced, NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Performance,
                              NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_UltraPerformance})
            NVSDK_NGX_Parameter_SetUI(g_dlss.params, p, preset);
        NVSDK_NGX_DLSS_Create_Params cp{};
        cp.Feature.InWidth = inW;
        cp.Feature.InHeight = inH;
        cp.Feature.InTargetWidth = f.output.width;
        cp.Feature.InTargetHeight = f.output.height;
        const NVSDK_NGX_PerfQuality_Value quality = dlssQuality(inW, f.output.width);
        cp.Feature.InPerfQualityValue = quality;
        cp.InFeatureCreateFlags = flags;
        NVSDK_NGX_Result r = NGX_VULKAN_CREATE_DLSS_EXT1(g_dev, f.cmd, 1, 1, &g_dlss.feature[e], g_dlss.params, &cp);
        if (NVSDK_NGX_FAILED(r)) {
            VRLOG("dlss: %s feature creation failed (0x%x)", slotName(slot), unsigned(r));
            g_dlss.feature[e] = nullptr;
            return false;
        }
        g_dlss.rw[e] = inW, g_dlss.rh[e] = inH, g_dlss.ow[e] = f.output.width, g_dlss.oh[e] = f.output.height;
        g_dlss.flags[e] = flags;
        g_reset[e] = true;
        VRLOG("dlss: %s %ux%u -> %ux%u (quality %d, preset %c)", slotName(slot), inW, inH, f.output.width, f.output.height, int(quality),
              preset ? char('A' + preset - 1) : '0');
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
    ep.InRenderSubrectDimensions = {inW, inH};  // at (0,0) of the colour, depth and motion images
    ep.InReset = (g_reset[e] || f.reset) ? 1 : 0;
    ep.InMVScaleX = f.motionScaleX;
    ep.InMVScaleY = f.motionScaleY;
    ep.InFrameTimeDeltaInMsec = f.frameMs;
    NVSDK_NGX_Result r = NGX_VULKAN_EVALUATE_DLSS_EXT(f.cmd, g_dlss.feature[e], g_dlss.params, &ep);
    g_reset[e] = false;
    if (NVSDK_NGX_FAILED(r)) {
        static int logged;
        if (logged++ < 4) VRLOG("dlss: %s evaluate failed (0x%x)", slotName(slot), unsigned(r));
        return false;
    }
    return true;
}

// ---------------- FSR 3.1 (FidelityFX API, Vulkan) ----------------
struct Fsr {
    bool ready = false;
    HMODULE dll = nullptr;
    ffxFunctions fn{};
    ffxContext ctx[kSlots] = {};
    uint32_t maxRender[kSlots][2] = {}, maxOut[kSlots][2] = {};
    uint32_t flags[kSlots] = {};
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
    g_fsr.ready = true;
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

bool fsrEvaluate(int slot, const Frame& f) {
    const int e = slot;
    const uint32_t inW = f.renderW ? f.renderW : f.color.width, inH = f.renderH ? f.renderH : f.color.height;
    uint32_t flags = FFX_UPSCALE_ENABLE_AUTO_EXPOSURE;
    if (f.hdr) flags |= FFX_UPSCALE_ENABLE_HIGH_DYNAMIC_RANGE;
    if (f.depthInverted) flags |= FFX_UPSCALE_ENABLE_DEPTH_INVERTED;
    if (g_fsrDepth == 2) flags |= FFX_UPSCALE_ENABLE_DEPTH_INFINITE;
    if (!g_fsr.ctx[e] || g_fsr.maxRender[e][0] < inW || g_fsr.maxRender[e][1] < inH ||
        g_fsr.maxOut[e][0] != f.output.width || g_fsr.maxOut[e][1] != f.output.height || g_fsr.flags[e] != flags) {
        if (g_fsr.ctx[e]) waitGpuIdle(), g_fsr.fn.DestroyContext(&g_fsr.ctx[e], nullptr), g_fsr.ctx[e] = nullptr;
        ffxCreateBackendVKDesc be{};
        be.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_VK;
        be.vkDevice = g_dev;
        be.vkPhysicalDevice = g_pd;
        be.vkDeviceProcAddr = g_gdpa;
        ffxCreateContextDescUpscale cd{};
        cd.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE;
        cd.header.pNext = &be.header;
        cd.flags = flags;
        cd.maxRenderSize = {inW, inH};
        cd.maxUpscaleSize = {f.output.width, f.output.height};
        ffxReturnCode_t rc = g_fsr.fn.CreateContext(&g_fsr.ctx[e], &cd.header, nullptr);
        if (rc != FFX_API_RETURN_OK) {
            VRLOG("fsr: %s context creation failed (%u)", slotName(slot), unsigned(rc));
            g_fsr.ctx[e] = nullptr;
            return false;
        }
        g_fsr.maxRender[e][0] = inW, g_fsr.maxRender[e][1] = inH;
        g_fsr.maxOut[e][0] = f.output.width, g_fsr.maxOut[e][1] = f.output.height;
        g_fsr.flags[e] = flags;
        g_reset[e] = true;
        VRLOG("fsr: %s %ux%u -> %ux%u (quality %d, near %.4g far %.4g, depth mode %d)", slotName(slot), inW, inH, f.output.width,
              f.output.height, g_quality, f.nearZ, f.farZ, g_fsrDepth);
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
    d.renderSize = {inW, inH};  // at (0,0); the resource descriptions keep the image extents
    d.upscaleSize = {f.output.width, f.output.height};
    d.enableSharpening = false;  // the mod's own RCAS pass sharpens after either backend (up::sharpness)
    d.sharpness = 0.0f;
    d.frameTimeDelta = f.frameMs;
    d.preExposure = 1.0f;
    d.reset = g_reset[e] || f.reset;
    d.cameraNear = f.depthInverted && g_fsrDepth == 0 ? f.farZ : f.nearZ;
    d.cameraFar = f.depthInverted && g_fsrDepth == 0 ? f.nearZ : f.farZ;
    if (g_fsrDepth == 2) d.cameraNear = FLT_MAX, d.cameraFar = f.nearZ;
    d.cameraFovAngleVertical = f.fovY;
    d.viewSpaceToMetersFactor = 1.0f;
    ffxReturnCode_t rc = g_fsr.fn.Dispatch(&g_fsr.ctx[e], &d.header);
    g_reset[e] = false;
    if (rc != FFX_API_RETURN_OK) {
        static int logged;
        if (logged++ < 4) VRLOG("fsr: %s dispatch failed (%u)", slotName(slot), unsigned(rc));
        return false;
    }
    return true;
}

uint32_t vendorId(VkInstance inst, PFN_vkGetInstanceProcAddr gipa, VkPhysicalDevice pd) {
    auto gp = reinterpret_cast<PFN_vkGetPhysicalDeviceProperties>(gipa(inst, "vkGetPhysicalDeviceProperties"));
    VkPhysicalDeviceProperties p{};
    if (gp) gp(pd, &p);
    return p.vendorID;
}
bool isNvidia(VkInstance inst, PFN_vkGetInstanceProcAddr gipa, VkPhysicalDevice pd) { return vendorId(inst, gipa, pd) == 0x10DE; }

// Desktop menu mode (docs/re/R28_rescaling_filter_menu.md): every backend this GPU supports is brought up so the menu
// can switch at runtime; the startup choice is the menu's saved one (or option upscaler, see src/se4/gfx_menu.cpp).
bool initMenu(VkInstance inst, PFN_vkGetInstanceProcAddr gipa, VkPhysicalDevice pd, const wchar_t* dir) {
    const uint32_t vendor = vendorId(inst, gipa, pd);
    if (vendor != 0x10DE) VRLOG("upscaler: DLSS not offered: not an NVIDIA GPU (vendor 0x%04x)", vendor);
    else if (!dlssInit(gipa, dir)) VRLOG("upscaler: DLSS not offered: NGX / DLSS unavailable (see the dlss: line above)");
    if (!fsrInit(dir)) VRLOG("upscaler: FSR 3.1 not offered (see the fsr: line above)");
    const int opt = g_menuStart != -2 ? g_menuStart : se4::options::get("upscaler", 0);
    const bool dlss = g_dlss.ready, fsr = g_fsr.ready;
    Kind k = Kind::Off;
    if (opt == 1) k = fsr ? Kind::FSR : dlss ? Kind::DLSS : Kind::Off;
    else if (opt != 0) k = dlss ? Kind::DLSS : fsr ? Kind::FSR : Kind::Off;  // DLSS or auto
    g_kind = k;
    VRLOG("upscaler: %s menu mode: DLSS %s, FSR 3.1 %s; start %s (requested %d; quality from %s, sharpness %.2f)", se4::g_vrEnabled ? "VR" : "desktop", dlss ? "available" : "unavailable",
          fsr ? "available" : "unavailable", name(), opt,
          se4::g_vrEnabled ? "the VR render resolution row" : se4::options::has("upscale_quality") ? "upscale_quality" : "the engine's Max 3D rendering Mpix",
          g_sharpness.load());
    return dlss || fsr;
}
}  // namespace

// Desktop: option desktop_upscaler. VR (R29): on unless option vr_upscaler_menu=0 or single_pass=1 forces single-pass
// stereo (the upscalers and the VR render resolution need two-pass).
bool menuMode() {
    if (!se4::g_vrEnabled) return se4::options::get("desktop_upscaler", 0) != 0;
    return se4::options::get("vr_upscaler_menu", 1) != 0 && se4::options::get("single_pass", 0) == 0;
}
int menuStart() { return g_menuStart; }
bool wanted() { return se4::options::get("upscaler", 0) != 0 || menuMode(); }
void setMenuStart(int opt) { g_menuStart = std::clamp(opt, -1, 2); }

void instanceExtensions(const char** names, uint32_t& count, uint32_t max) {
    if (!wanted() || !ngx::load()) return;
    auto req = reinterpret_cast<decltype(&NVSDK_NGX_VULKAN_RequiredExtensions)>(ngx::sym("NVSDK_NGX_VULKAN_RequiredExtensions"));
    unsigned int ic = 0, dc = 0;
    const char **ie = nullptr, **de = nullptr;
    if (!req || NVSDK_NGX_FAILED(req(&ic, &ie, &dc, &de))) return;
    for (unsigned i = 0; i < ic && count < max; ++i) names[count++] = ie[i];
}

void deviceExtensions(VkInstance, VkPhysicalDevice, const char** names, uint32_t& count, uint32_t max) {
    if (!wanted() || !ngx::load()) return;
    auto req = reinterpret_cast<decltype(&NVSDK_NGX_VULKAN_RequiredExtensions)>(ngx::sym("NVSDK_NGX_VULKAN_RequiredExtensions"));
    unsigned int ic = 0, dc = 0;
    const char **ie = nullptr, **de = nullptr;
    if (!req || NVSDK_NGX_FAILED(req(&ic, &ie, &dc, &de))) return;
    for (unsigned i = 0; i < dc && count < max; ++i) names[count++] = de[i];
}

bool init(VkInstance inst, VkPhysicalDevice pd, VkDevice dev, PFN_vkGetInstanceProcAddr gipa, PFN_vkGetDeviceProcAddr gdpa, VkQueue queue,
          uint32_t, const wchar_t* dir) {
    g_inst = inst, g_pd = pd, g_dev = dev, g_gdpa = gdpa, g_queue = queue;
    int opt = se4::options::get("upscaler", 0);  // off until validated in the lab; auto = -1
    g_quality = std::clamp(se4::options::get("upscale_quality", 0), 0, 3);
    if (!g_sharpnessSet) g_sharpness = std::clamp(se4::options::get("upscale_sharpness", 25), 0, 100) / 100.0f;
    g_fsrDepth = std::clamp(se4::options::get("upscale_fsr_depth", 0), 0, 2);
    if (menuMode()) return initMenu(inst, gipa, pd, dir);
    if (opt == 0) {
        VRLOG("upscaler: off");
        return false;
    }
    const bool nvidia = isNvidia(inst, gipa, pd);
    if ((opt == 2 || (opt < 0 && nvidia)) && dlssInit(gipa, dir)) g_kind = Kind::DLSS;
    else if (opt != 2 && fsrInit(dir)) g_kind = Kind::FSR;
    else if (opt == 2 && fsrInit(dir)) g_kind = Kind::FSR;  // DLSS requested but unavailable
    VRLOG("upscaler: %s (quality %d, sharpness %.2f)", name(), g_quality, g_sharpness.load());
    return g_kind != Kind::Off;
}

Kind active() { return g_kind; }
const char* name(Kind k) { return k == Kind::DLSS ? "DLSS" : k == Kind::FSR ? "FSR 3.1" : "off"; }
const char* name() { return name(g_kind); }

bool available(Kind k) { return k == Kind::Off || (k == Kind::DLSS && g_dlss.ready) || (k == Kind::FSR && g_fsr.ready); }

bool select(Kind k) {
    if (!available(k)) return false;
    const Kind was = g_kind.exchange(k);
    if (was != k) {
        resetHistory();
        VRLOG("upscaler: %s -> %s (histories reset)", name(was), name(k));
    }
    return true;
}

float ratio() { return g_kind == Kind::Off ? 1.0f : core::upscaleRatio(g_quality); }

void renderSize(uint32_t outW, uint32_t outH, uint32_t& rw, uint32_t& rh) { core::upscaleRenderSize(outW, outH, ratio(), rw, rh); }

void jitter(uint32_t frame, uint32_t renderW, uint32_t outW, float& px, float& py) {
    if (g_kind == Kind::Off || !renderW) {
        px = py = 0;
        return;
    }
    core::jitterOffset(frame, renderW, outW, px, py);
}

float mipBias(uint32_t renderW, uint32_t outW) { return g_kind == Kind::Off ? 0.0f : core::upscaleMipBias(renderW, outW); }

bool evaluate(int slot, const Frame& f) {
    if (slot < 0 || slot >= kSlots) return false;
    const Kind k = g_kind;
    if (k == Kind::DLSS) return dlssEvaluate(slot, f);
    if (k == Kind::FSR) return fsrEvaluate(slot, f);
    return false;
}

float sharpness() { return g_sharpness.load(std::memory_order_relaxed); }
void setSharpness(float s) {
    g_sharpnessSet = true;
    g_sharpness = std::clamp(s, 0.0f, 1.0f);
}

void resetHistory() { g_reset[0] = g_reset[1] = g_reset[2] = true; }

void waitGpuIdle() {
    static PFN_vkQueueWaitIdle wait;
    if (!wait && g_gdpa && g_dev) wait = reinterpret_cast<PFN_vkQueueWaitIdle>(g_gdpa(g_dev, "vkQueueWaitIdle"));
    if (!wait || !g_queue) return;
    vkq::Guard lock;
    wait(g_queue);
}
}  // namespace up
