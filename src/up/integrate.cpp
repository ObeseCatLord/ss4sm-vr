#include "up/integrate.hpp"
#include "core/reproject.hpp"
#include "se4/build.hpp"
#include "se4/log.hpp"
#include "se4/options.hpp"
#include "se4/patch.hpp"
#include "se4/player_vr.hpp"
#include "up/upscaler.hpp"
#include "xr/runtime.hpp"
#include <windows.h>
#include <MinHook.h>
#include <cstdlib>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <unordered_set>

namespace upint {
namespace {
const uint32_t kMotionSpv[] =
#include "motion.comp.inc"
    ;

using HdrFn = void*(__cdecl*)(uint32_t a);
using PpFn = void(__cdecl*)(void* gfx, void* dst, uint32_t flags, void* p4);
using HeadWorldFn = void*(__cdecl*)(void* vrd, float* out);
HdrFn realHdr;
PpFn realPp;

bool g_evaluate;  // an upscaler is selected (options)
int g_trace;      // lab: trace this many projection calls once the upscaler runs (upscale_trace)
DWORD g_hdrThread;
// The engine's Vulkan graphics object keeps the command buffer it records into at +0x1ea8 (R13 s5; lab 2026-10-24:
// the PP's own canvas barrier goes into exactly that handle; 58 identical loads in Sam4 616154 and SamSM 628492).
// It is not bound to the calling thread, so it is read from there and checked for the loader's dispatch magic.
constexpr size_t kGfxCommandBuffer = 0x1ea8;
constexpr uintptr_t kLoaderMagic = 0x01CDC0DE;
std::mutex g_passLock;
std::unordered_set<VkCommandBuffer> g_openPass;  // command buffers inside a render pass
thread_local int t_ppDepth;  // inside realPp
thread_local uint32_t t_ppFlags;
thread_local VkCommandBuffer t_ppCb;
thread_local int t_pendingEye;  // evaluate this eye at the PP's first canvas barrier
// PP flags of the per-eye dispatch whose post-processing reads the canvas (learnt from its first barrier).
volatile LONG g_ppFlags = -1;

struct Captured {
    VkImage image = VK_NULL_HANDLE;
    VkFormat format = VK_FORMAT_UNDEFINED;
    uint32_t w = 0, h = 0;
} g_color, g_depth;

bool isDepth(VkFormat f) {
    return f == VK_FORMAT_D16_UNORM || f == VK_FORMAT_D32_SFLOAT || f == VK_FORMAT_D24_UNORM_S8_UINT ||
           f == VK_FORMAT_D32_SFLOAT_S8_UINT || f == VK_FORMAT_X8_D24_UNORM_PACK32 || f == VK_FORMAT_D16_UNORM_S8_UINT;
}

// ---- Vulkan ----
struct Fn {
    VkDevice dev = VK_NULL_HANDLE;
    PFN_vkGetDeviceProcAddr gdpa = nullptr;
    VkPhysicalDeviceMemoryProperties mem{};
    PFN_vkCreateImage createImage;
    PFN_vkDestroyImage destroyImage;
    PFN_vkCreateImageView createView;
    PFN_vkDestroyImageView destroyView;
    PFN_vkGetImageMemoryRequirements memReq;
    PFN_vkAllocateMemory alloc;
    PFN_vkFreeMemory freeMem;
    PFN_vkBindImageMemory bind;
    PFN_vkCreateSampler createSampler;
    PFN_vkCreateDescriptorSetLayout createDsl;
    PFN_vkCreatePipelineLayout createPl;
    PFN_vkCreateShaderModule createShader;
    PFN_vkDestroyShaderModule destroyShader;
    PFN_vkCreateComputePipelines createCompute;
    PFN_vkCreateDescriptorPool createPool;
    PFN_vkAllocateDescriptorSets allocSets;
    PFN_vkFreeDescriptorSets freeSets;
    PFN_vkUpdateDescriptorSets updateSets;
    PFN_vkCmdPipelineBarrier barrier;
    PFN_vkCmdBindPipeline bindPipeline;
    PFN_vkCmdBindDescriptorSets bindSets;
    PFN_vkCmdPushConstants push;
    PFN_vkCmdDispatch dispatch;
    PFN_vkCmdCopyImage copy;
    PFN_vkCmdClearColorImage clear;
} vk;

struct OwnImage {
    VkImage image = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    VkDeviceMemory mem = VK_NULL_HANDLE;
};

struct Resources {
    bool ready = false, failed = false;
    VkImage color = VK_NULL_HANDLE, depth = VK_NULL_HANDLE;  // the engine images these were made for
    uint32_t w = 0, h = 0;
    VkImageView colorView = VK_NULL_HANDLE, depthView = VK_NULL_HANDLE;
    OwnImage motion, output;
    VkDescriptorSet set = VK_NULL_HANDLE;
} g_res;

// Created once per device.
VkSampler g_sampler;
VkDescriptorSetLayout g_dsl;
VkPipelineLayout g_pl;
VkPipeline g_pipe;
VkDescriptorPool g_pool;

uint32_t memType(uint32_t bits) {
    for (uint32_t i = 0; i < vk.mem.memoryTypeCount; ++i)
        if ((bits & (1u << i)) && (vk.mem.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) return i;
    return UINT32_MAX;
}

bool createOwn(OwnImage& o, uint32_t w, uint32_t h, VkImageUsageFlags usage) {
    VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ci.imageType = VK_IMAGE_TYPE_2D;
    ci.format = VK_FORMAT_R16G16B16A16_SFLOAT;
    ci.extent = {w, h, 1};
    ci.mipLevels = ci.arrayLayers = 1;
    ci.samples = VK_SAMPLE_COUNT_1_BIT;
    ci.usage = usage;
    if (vk.createImage(vk.dev, &ci, nullptr, &o.image) != VK_SUCCESS) return false;
    VkMemoryRequirements mr;
    vk.memReq(vk.dev, o.image, &mr);
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.allocationSize = mr.size;
    ai.memoryTypeIndex = memType(mr.memoryTypeBits);
    if (ai.memoryTypeIndex == UINT32_MAX || vk.alloc(vk.dev, &ai, nullptr, &o.mem) != VK_SUCCESS) return false;
    if (vk.bind(vk.dev, o.image, o.mem, 0) != VK_SUCCESS) return false;
    VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vi.image = o.image;
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vi.format = ci.format;
    vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    return vk.createView(vk.dev, &vi, nullptr, &o.view) == VK_SUCCESS;
}

void destroyOwn(OwnImage& o) {
    if (o.view) vk.destroyView(vk.dev, o.view, nullptr);
    if (o.image) vk.destroyImage(vk.dev, o.image, nullptr);
    if (o.mem) vk.freeMem(vk.dev, o.mem, nullptr);
    o = {};
}

VkImageView viewOf(VkImage image, VkFormat format, VkImageAspectFlags aspect) {
    VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vi.image = image;
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vi.format = format;
    vi.subresourceRange = {aspect, 0, 1, 0, 1};
    VkImageView v = VK_NULL_HANDLE;
    vk.createView(vk.dev, &vi, nullptr, &v);
    return v;
}

bool createPipeline() {
    VkSamplerCreateInfo si{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    si.magFilter = si.minFilter = VK_FILTER_NEAREST;
    si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    if (vk.createSampler(vk.dev, &si, nullptr, &g_sampler) != VK_SUCCESS) return false;
    VkDescriptorSetLayoutBinding b[2] = {};
    b[0] = {0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, &g_sampler};
    b[1] = {1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
    VkDescriptorSetLayoutCreateInfo dl{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    dl.bindingCount = 2;
    dl.pBindings = b;
    if (vk.createDsl(vk.dev, &dl, nullptr, &g_dsl) != VK_SUCCESS) return false;
    VkPushConstantRange pr{VK_SHADER_STAGE_COMPUTE_BIT, 0, 128};
    VkPipelineLayoutCreateInfo pl{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pl.setLayoutCount = 1;
    pl.pSetLayouts = &g_dsl;
    pl.pushConstantRangeCount = 1;
    pl.pPushConstantRanges = &pr;
    if (vk.createPl(vk.dev, &pl, nullptr, &g_pl) != VK_SUCCESS) return false;
    VkShaderModuleCreateInfo sm{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    sm.codeSize = sizeof(kMotionSpv);
    sm.pCode = kMotionSpv;
    VkShaderModule mod;
    if (vk.createShader(vk.dev, &sm, nullptr, &mod) != VK_SUCCESS) return false;
    VkComputePipelineCreateInfo cp{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    cp.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_COMPUTE_BIT, mod, "main", nullptr};
    cp.layout = g_pl;
    VkResult r = vk.createCompute(vk.dev, VK_NULL_HANDLE, 1, &cp, nullptr, &g_pipe);
    vk.destroyShader(vk.dev, mod, nullptr);
    if (r != VK_SUCCESS) return false;
    VkDescriptorPoolSize ps[2] = {{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 4}, {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 4}};
    VkDescriptorPoolCreateInfo dp{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    dp.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
    dp.maxSets = 4;
    dp.poolSizeCount = 2;
    dp.pPoolSizes = ps;
    return vk.createPool(vk.dev, &dp, nullptr, &g_pool) == VK_SUCCESS;
}

// Called when the engine has re-created its HDR targets: the old canvas/depth are gone, so are any command buffers
// that used our views of them.
void releaseResources() {
    if (g_res.set) vk.freeSets(vk.dev, g_pool, 1, &g_res.set);
    if (g_res.colorView) vk.destroyView(vk.dev, g_res.colorView, nullptr);
    if (g_res.depthView) vk.destroyView(vk.dev, g_res.depthView, nullptr);
    destroyOwn(g_res.motion);
    destroyOwn(g_res.output);
    g_res = {};
}

bool ensureResources() {
    if (g_res.color == g_color.image && g_res.depth == g_depth.image && (g_res.ready || g_res.failed)) return g_res.ready;
    if (!vk.dev || !g_color.image || !g_depth.image || g_color.w != g_depth.w || g_color.h != g_depth.h) return false;
    if (!g_pipe && !createPipeline()) {
        VRLOG("upint: motion pipeline creation failed");
        g_evaluate = false;
        return false;
    }
    releaseResources();
    g_res.color = g_color.image, g_res.depth = g_depth.image;
    g_res.w = g_color.w, g_res.h = g_color.h;
    g_res.colorView = viewOf(g_color.image, g_color.format, VK_IMAGE_ASPECT_COLOR_BIT);
    g_res.depthView = viewOf(g_depth.image, g_depth.format, VK_IMAGE_ASPECT_DEPTH_BIT);
    bool ok = g_res.colorView && g_res.depthView &&
              createOwn(g_res.motion, g_res.w, g_res.h, VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT) &&
              createOwn(g_res.output, g_res.w, g_res.h,
                        VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
    if (ok) {
        VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        ai.descriptorPool = g_pool;
        ai.descriptorSetCount = 1;
        ai.pSetLayouts = &g_dsl;
        ok = vk.allocSets(vk.dev, &ai, &g_res.set) == VK_SUCCESS;
    }
    if (ok) {
        // One set for both eyes: its images never change while it is in use, so recorded command buffers stay valid.
        VkDescriptorImageInfo di{g_sampler, g_res.depthView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        VkDescriptorImageInfo mi{VK_NULL_HANDLE, g_res.motion.view, VK_IMAGE_LAYOUT_GENERAL};
        VkWriteDescriptorSet w[2] = {};
        w[0] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, g_res.set, 0, 0, 1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &di};
        w[1] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, g_res.set, 1, 0, 1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, &mi};
        vk.updateSets(vk.dev, 2, w, 0, nullptr);
    }
    g_res.ready = ok;
    g_res.failed = !ok;
    if (ok && se4::options::get("upscale_trace", 0)) g_trace = 60;
    VRLOG("upint: resources for canvas %ux%u %s", g_res.w, g_res.h, ok ? "ready" : "FAILED");
    up::resetHistory();
    return ok;
}

// ---- per-eye camera state ----
struct ProjRec {
    float appliedX = 0, appliedY = 0;  // jitter of the last projection handed to the engine for this eye
    uint32_t appliedFrame = 0;
    core::Tangents tan{};
    float n = 0.1f, f = 1000.0f;
    bool valid = false;
};
struct EyeHistory {
    bool valid = false;
    uint32_t frame = 0;
    core::Pose worldEye{};
    core::Tangents tan{};
};
ProjRec g_proj[2];
EyeHistory g_hist[2];
uint32_t g_frame;
int64_t g_frameTime;
float g_jx, g_jy;
// Lab switches: the sign of the jitter reported to the upscaler (upscale_jitter_x/y), rendering without jitter
// (upscale_jitter=0), and jitter without the upscaler (upscale_evaluate=0).
float g_jitterSign[2] = {1, 1};
bool g_jitterOn = true;
bool g_evaluateOn = true;
float g_jitterScale = 1.0f;
bool g_motionOn = true;
float g_reportScale = 1.0f;
int g_jitterLag;  // lab: upscale_jitter_lag reports the jitter of frame (current - lag)  // lab: upscale_report_scale (percent) scales the jitter reported to the upscaler  // lab: upscale_mv=0 clears the motion vectors instead of computing them  // lab: upscale_jitter_scale (percent) magnifies the rendered jitter for visibility checks
LARGE_INTEGER g_lastEval[2];

float worldScale() {
    auto* s = se4::build().at<float>(se4::gen::vr_fWorldScale);
    return (s && *s > 0.01f) ? *s : 1.0f;
}

// World placement of an eye camera in engine units: head world (vrd anchor o device slot 0x80) o eye in head.
bool worldEye(int eye, core::Pose& out) {
    void* vrd = player_vr::localVrd();
    auto fn = se4::build().at<void>(se4::gen::vrd_head_world);
    const auto& v = xr::runtime().views();
    if (!vrd || !fn || !v.valid) return false;
    float p7[7] = {};
    reinterpret_cast<HeadWorldFn>(fn)(vrd, p7);
    core::Pose head{core::normalize({p7[0], p7[1], p7[2], p7[3]}), {p7[4], p7[5], p7[6]}};
    const XrPosef& e = v.inView[eye - 1].pose;
    const float s = 1.0f / worldScale();
    core::Pose inHead{{e.orientation.x, e.orientation.y, e.orientation.z, e.orientation.w},
                      {e.position.x * s, e.position.y * s, e.position.z * s}};
    out = core::compose(head, inHead);
    return core::finite(out);
}

void imageBarrier(VkImageMemoryBarrier& b, VkImage img, VkImageAspectFlags aspect, VkImageLayout from, VkImageLayout to,
                  VkAccessFlags src, VkAccessFlags dst) {
    b = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    b.srcAccessMask = src;
    b.dstAccessMask = dst;
    b.oldLayout = from;
    b.newLayout = to;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = img;
    b.subresourceRange = {aspect, 0, 1, 0, 1};
}

void evaluateEye(VkCommandBuffer cb, int eye) {
    const int e = eye - 1;
    if (!ensureResources() || !g_proj[e].valid) return;
    core::Pose cur;
    if (!worldEye(eye, cur)) {
        g_hist[e].valid = false;
        return;
    }
    const uint32_t W = g_res.w, H = g_res.h;
    EyeHistory& h = g_hist[e];
    bool reset = !h.valid || h.frame + 1 != g_frame;
    core::Pose c2p{};
    if (!reset) {
        c2p = core::currentToPrevious(h.worldEye, cur);
        const float moved = std::sqrt(core::dot(c2p.p, c2p.p)) * worldScale();  // metres
        const float turned = 2 * std::acos(std::min(1.0f, std::fabs(c2p.q.w)));
        if (moved > 1.0f || turned > 0.6f) reset = true;  // teleport / respawn / camera cut
    }
    if (reset) c2p = {};
    {
        // Lab diagnostics: camera motion per frame and history resets, once a second per eye.
        static float maxMove[2], maxTurn[2];
        static int resets[2], evals[2];
        static LARGE_INTEGER last[2];
        const float moved = std::sqrt(core::dot(c2p.p, c2p.p)) * worldScale() * 1000.0f;  // mm
        const float turned = 2 * std::acos(std::min(1.0f, std::fabs(c2p.q.w))) * 180.0f / core::Pi;
        maxMove[e] = std::max(maxMove[e], moved);
        maxTurn[e] = std::max(maxTurn[e], turned);
        resets[e] += reset;
        ++evals[e];
        LARGE_INTEGER t, fq;
        QueryPerformanceCounter(&t);
        QueryPerformanceFrequency(&fq);
        if (t.QuadPart - last[e].QuadPart > fq.QuadPart) {
            if (last[e].QuadPart)
                VRLOG("upint: eye %d %d evaluations, %d resets, camera motion max %.2f mm %.3f deg per frame", eye, evals[e], resets[e],
                      maxMove[e], maxTurn[e]);
            last[e] = t;
            maxMove[e] = maxTurn[e] = 0;
            resets[e] = evals[e] = 0;
        }
    }

    // 1. canvas and depth to shader-read, our images to GENERAL.
    VkImageMemoryBarrier b[4];
    imageBarrier(b[0], g_res.color, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                 VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT);
    imageBarrier(b[1], g_res.depth, VK_IMAGE_ASPECT_DEPTH_BIT, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
                 VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT);
    imageBarrier(b[2], g_res.motion.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL, 0,
                 VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT);
    imageBarrier(b[3], g_res.output.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL, 0,
                 VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT);
    vk.barrier(cb,
               VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT | VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT |
                   VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
               VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 4, b);

    // 2. camera motion vectors.
    struct {
        float curToPrev[16];
        float cur[4], prev[4], size[4], origin[4];
    } pc{};
    core::rigidMatrix(c2p, pc.curToPrev);
    const core::Tangents& ct = g_proj[e].tan;
    const core::Tangents& pt = reset ? ct : h.tan;
    memcpy(pc.cur, &ct, sizeof pc.cur);
    memcpy(pc.prev, &pt, sizeof pc.prev);
    pc.size[0] = float(W), pc.size[1] = float(H), pc.size[2] = g_proj[e].n, pc.size[3] = g_proj[e].f;
    if (g_motionOn) {
        vk.bindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, g_pipe);
        vk.bindSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, g_pl, 0, 1, &g_res.set, 0, nullptr);
        vk.push(cb, g_pl, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof pc, &pc);
        vk.dispatch(cb, (W + 7) / 8, (H + 7) / 8, 1);
    } else {
        const VkClearColorValue zero{};
        const VkImageSubresourceRange all{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vk.clear(cb, g_res.motion.image, VK_IMAGE_LAYOUT_GENERAL, &zero, 1, &all);
    }
    imageBarrier(b[0], g_res.motion.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                 VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT);
    vk.barrier(cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, 0,
               nullptr, 1, b);

    // 3. the upscaler (phase A, native AA: render size == output size).
    LARGE_INTEGER now, freq;
    QueryPerformanceCounter(&now);
    QueryPerformanceFrequency(&freq);
    float ms = g_lastEval[e].QuadPart ? float(double(now.QuadPart - g_lastEval[e].QuadPart) * 1000.0 / double(freq.QuadPart)) : 11.1f;
    g_lastEval[e] = now;
    up::Frame f;
    f.cmd = cb;
    f.color = {g_res.color, g_res.colorView, g_color.format, W, H};
    f.depth = {g_res.depth, g_res.depthView, g_depth.format, W, H};
    f.motion = {g_res.motion.image, g_res.motion.view, VK_FORMAT_R16G16B16A16_SFLOAT, W, H};
    f.output = {g_res.output.image, g_res.output.view, VK_FORMAT_R16G16B16A16_SFLOAT, W, H};
    float rx = g_jx, ry = g_jy;
    if (g_jitterLag) up::jitter(uint32_t(int(g_frame) - g_jitterLag), g_color.w, g_color.w, rx, ry);  // lab: report another frame's jitter
    f.jitterX = rx * g_jitterSign[0] * g_reportScale, f.jitterY = ry * g_jitterSign[1] * g_reportScale;
    {
        static int mismatches, checks;
        ++checks;
        if (g_proj[e].appliedFrame != g_frame || g_proj[e].appliedX != g_jx || g_proj[e].appliedY != g_jy) ++mismatches;
        if (checks % 600 == 0) VRLOG("upint: jitter check: %d of %d evaluations saw a different jitter than the eye's last projection", mismatches, checks);
    }
    f.frameMs = std::clamp(ms, 1.0f, 100.0f);
    f.nearZ = g_proj[e].n, f.farZ = g_proj[e].f;
    f.fovY = std::atan(ct.u) - std::atan(ct.d);
    f.depthInverted = true;
    f.hdr = true;
    f.reset = reset;
    const bool ok = up::evaluate(eye, f);

    // 4. copy back and restore the engine's layouts.
    VkPipelineStageFlags restoreSrc = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
    uint32_t n = 0;
    if (ok) {
        imageBarrier(b[0], g_res.output.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                     VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT);
        imageBarrier(b[1], g_res.color, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                     VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
        vk.barrier(cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 2, b);
        VkImageCopy rc{};
        rc.srcSubresource = rc.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        rc.extent = {W, H, 1};
        vk.copy(cb, g_res.output.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, g_res.color, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &rc);
        imageBarrier(b[n++], g_res.color, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                     VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT,
                     VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_SHADER_READ_BIT);
        restoreSrc |= VK_PIPELINE_STAGE_TRANSFER_BIT;
    } else {
        imageBarrier(b[n++], g_res.color, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                     VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_ACCESS_SHADER_READ_BIT,
                     VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_SHADER_READ_BIT);
    }
    imageBarrier(b[n++], g_res.depth, VK_IMAGE_ASPECT_DEPTH_BIT, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                 VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL, VK_ACCESS_SHADER_READ_BIT,
                 VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT);
    vk.barrier(cb, restoreSrc,
               VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT |
                   VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
               0, 0, nullptr, 0, nullptr, n, b);

    h.valid = ok;
    h.frame = g_frame;
    h.worldEye = cur;
    h.tan = ct;
    if (g_trace > 0) VRLOG("upint: trace evaluate eye %d frame %u jitter %.3f %.3f", eye, g_frame, g_jx, g_jy);
    static volatile LONG logged;
    if (InterlockedIncrement(&logged) <= 4)
        VRLOG("upint: eye %d %s %ux%u (frame %u, jitter %.3f %.3f, reset %d)", eye, ok ? "evaluated" : "evaluate FAILED", W, H, g_frame,
              g_jx, g_jy, int(reset));
}

// ---- engine hooks ----
void* hookedHdr(uint32_t a) {
    g_hdrThread = GetCurrentThreadId();
    void* r = realHdr(a);
    g_hdrThread = 0;
    return r;
}

VkCommandBuffer engineCommandBuffer(void* gfx) {
    if (!gfx) return VK_NULL_HANDLE;
    auto cb = *reinterpret_cast<VkCommandBuffer*>(static_cast<uint8_t*>(gfx) + kGfxCommandBuffer);
    if (!cb || (*reinterpret_cast<uintptr_t*>(cb) & 0xffffffffu) != kLoaderMagic) return VK_NULL_HANDLE;
    return cb;
}

// ---- Texture LOD bias while upscaling (docs/re/R23_lod_bias.md) ----
// A temporal upscaler needs a negative mip bias (log2(render/display) - 1: -1 for DLAA / FSR native AA) or fine
// texture detail washes out. The engine's two scene texture binds load tex_fLODBias into the sampler state; both loads
// jump to stubs that add g_extraBias (the cvar itself is never written). g_extraBias is set only while an eye's scene
// executes with the upscaler evaluating, and cleared before its post-processing.
volatile float* g_extraBias;
float g_lodBias = -1.0f;  // option upscale_lod_bias (hundredths), default -100
using ExecFn = void(__cdecl*)(void* a1, void* a2, void* a3, void* a4);
ExecFn realExecute;
bool upscaling() { return g_evaluate && g_evaluateOn && g_jitterOn && up::active() != up::Kind::Off; }
void __cdecl hookedExecute(void* a1, void* a2, void* a3, void* a4) {
    void** devp = se4::build().at<void*>(se4::gen::vr_device_global);
    const int eye = devp && *devp ? *reinterpret_cast<int32_t*>(static_cast<uint8_t*>(*devp) + 0x18) : -1;
    if (g_extraBias) *g_extraBias = (eye == 1 || eye == 2) && upscaling() ? g_lodBias : 0.0f;
    realExecute(a1, a2, a3, a4);
    if (g_extraBias) *g_extraBias = 0.0f;
}
bool installLodBias() {
    const auto& b = se4::build();
    auto* tex = b.at<uint8_t>(se4::gen::tex_fLODBias);
    void* exec = b.at<void>(se4::gen::execute_view);
    if (!tex || !exec || g_lodBias == 0.0f) return false;
    auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(b.base);
    auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(b.base + dos->e_lfanew);
    const IMAGE_SECTION_HEADER* sec = IMAGE_FIRST_SECTION(nt);
    uint8_t* lo = reinterpret_cast<uint8_t*>(b.base + sec->VirtualAddress);
    uint8_t* hi = lo + sec->Misc.VirtualSize - 16;
    // site 1: 74 05 0F 57 F6 EB 08 | F3 0F 10 35 <rel32 tex> (xmm6); site 2: 48 8D 53 0A | F3 0F 10 05 <rel32 tex> (xmm0)
    uint8_t *site[2] = {}, n[2] = {};
    const uint8_t pre1[7] = {0x74, 0x05, 0x0F, 0x57, 0xF6, 0xEB, 0x08}, pre2[4] = {0x48, 0x8D, 0x53, 0x0A};
    for (uint8_t* p = lo; p < hi; ++p) {
        if (p[0] != 0xF3 || p[1] != 0x0F || p[2] != 0x10 || (p[3] != 0x35 && p[3] != 0x05)) continue;
        if (p + 8 + *reinterpret_cast<int32_t*>(p + 4) != tex) continue;
        if (p[3] == 0x35 && p - 7 >= lo && !memcmp(p - 7, pre1, 7)) site[0] = p, ++n[0];
        if (p[3] == 0x05 && p - 4 >= lo && !memcmp(p - 4, pre2, 4)) site[1] = p, ++n[1];
    }
    if (n[0] != 1 || n[1] != 1) return VRLOG("upint: texture LOD bias loads found %d/%d, expected 1/1 - no bias", n[0], n[1]), false;
    uint8_t* blk = se4::patch::allocNear(site[0], 64);
    if (!blk || std::llabs(blk - site[1]) > 0x7fff0000ll) return VRLOG("upint: no stub memory for the LOD bias"), false;
    g_extraBias = reinterpret_cast<volatile float*>(blk + 56);
    *g_extraBias = 0.0f;
    for (int i = 0; i < 2; ++i) {
        uint8_t* st = blk + i * 24;
        const uint8_t reg = site[i][3];  // 0x35 xmm6 / 0x05 xmm0
        auto rel = [](uint8_t* from, const void* to) { return int32_t(static_cast<const uint8_t*>(to) - from); };
        uint8_t code[21] = {0xF3, 0x0F, 0x10, reg, 0, 0, 0, 0, 0xF3, 0x0F, 0x58, reg, 0, 0, 0, 0, 0xE9, 0, 0, 0, 0};
        int32_t r = rel(st + 8, tex);
        memcpy(code + 4, &r, 4);
        r = rel(st + 16, const_cast<float*>(g_extraBias));
        memcpy(code + 12, &r, 4);
        r = rel(st + 21, site[i] + 8);
        memcpy(code + 17, &r, 4);
        DWORD old;
        VirtualProtect(st, sizeof code, PAGE_EXECUTE_READWRITE, &old);
        memcpy(st, code, sizeof code);
        uint8_t expect[8], patch[8] = {0xE9, 0, 0, 0, 0, 0x90, 0x90, 0x90};
        memcpy(expect, site[i], 8);
        r = rel(site[i] + 5, st);
        memcpy(patch + 1, &r, 4);
        if (!se4::patch::writeCode(site[i], expect, patch, 8)) return VRLOG("upint: LOD bias patch FAILED at %p", static_cast<void*>(site[i])), false;
    }
    if (MH_CreateHook(exec, reinterpret_cast<void*>(hookedExecute), reinterpret_cast<void**>(&realExecute)) != MH_OK || MH_EnableHook(exec) != MH_OK)
        return VRLOG("upint: execute-view hook FAILED - LOD bias stays 0"), false;
    VRLOG("upint: texture LOD bias %+.2f while upscaling (binds %p, %p)", g_lodBias, static_cast<void*>(site[0]), static_cast<void*>(site[1]));
    return true;
}

void hookedPp(void* gfx, void* dst, uint32_t flags, void* p4) {
    if (g_extraBias) *g_extraBias = 0.0f;  // post-processing and what follows in the view are unbiased
    void** devp = se4::build().at<void*>(se4::gen::vr_device_global);
    const int eye = devp && *devp ? *reinterpret_cast<int32_t*>(static_cast<uint8_t*>(*devp) + 0x18) : -1;
    const VkCommandBuffer cb = engineCommandBuffer(gfx);
    // The scene pass is usually still open here (the engine ends passes lazily): the work is recorded at the PP's
    // own first canvas barrier instead (onImageBarrier), where no pass can be open.
    const bool want = g_evaluate && g_evaluateOn && up::active() != up::Kind::Off && (flags & 1) == 0 && LONG(flags) == g_ppFlags &&
                      (eye == 1 || eye == 2) && !t_ppDepth && cb && player_vr::localVrd();
    ++t_ppDepth;
    t_ppFlags = flags;
    t_ppCb = cb;
    t_pendingEye = want ? eye : 0;
    realPp(gfx, dst, flags, p4);
    if (t_pendingEye) {
        static volatile LONG logged;
        if (InterlockedIncrement(&logged) <= 3) VRLOG("upint: eye %d: no canvas barrier seen in post-processing", t_pendingEye);
    }
    t_pendingEye = 0;
    --t_ppDepth;
}
}  // namespace

void onDevice(VkInstance inst, PFN_vkGetInstanceProcAddr gipa, VkPhysicalDevice pd, VkDevice dev, PFN_vkGetDeviceProcAddr gdpa) {
    vk.dev = dev;
    vk.gdpa = gdpa;
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
    F(createSampler, vkCreateSampler);
    F(createDsl, vkCreateDescriptorSetLayout);
    F(createPl, vkCreatePipelineLayout);
    F(createShader, vkCreateShaderModule);
    F(destroyShader, vkDestroyShaderModule);
    F(createCompute, vkCreateComputePipelines);
    F(createPool, vkCreateDescriptorPool);
    F(allocSets, vkAllocateDescriptorSets);
    F(freeSets, vkFreeDescriptorSets);
    F(updateSets, vkUpdateDescriptorSets);
    F(barrier, vkCmdPipelineBarrier);
    F(bindPipeline, vkCmdBindPipeline);
    F(bindSets, vkCmdBindDescriptorSets);
    F(push, vkCmdPushConstants);
    F(dispatch, vkCmdDispatch);
    F(copy, vkCmdCopyImage);
    F(clear, vkCmdClearColorImage);
#undef F
}

void patchImage(VkImageCreateInfo& ci) {
    if (!g_evaluate || !g_hdrThread || g_hdrThread != GetCurrentThreadId() || !isDepth(ci.format)) return;
    ci.usage |= VK_IMAGE_USAGE_SAMPLED_BIT;
}

void onImageCreated(VkImage image, const VkImageCreateInfo& ci) {
    if (!g_hdrThread || g_hdrThread != GetCurrentThreadId() || ci.samples != VK_SAMPLE_COUNT_1_BIT) return;
    VRLOG("upint: HDR allocation created image %p %ux%u fmt %d usage %x", (void*)image, ci.extent.width, ci.extent.height,
          int(ci.format), ci.usage);
    Captured c{image, ci.format, ci.extent.width, ci.extent.height};
    if (isDepth(ci.format)) g_depth = c;
    else if (ci.format == VK_FORMAT_R16G16B16A16_SFLOAT || ci.format == VK_FORMAT_R32G32B32A32_SFLOAT) g_color = c;
}

template <class A> bool patchAttachments(const A* in, uint32_t n, std::vector<A>& out) {
    int depth = -1;
    bool hdrColor = false;
    for (uint32_t i = 0; i < n; ++i) {
        if (isDepth(in[i].format) && in[i].samples == VK_SAMPLE_COUNT_1_BIT) depth = int(i);
        if (in[i].format == VK_FORMAT_R16G16B16A16_SFLOAT) hdrColor = true;
    }
    // Lab 2026-10-24: every scene pass already stores depth (and keeps DEPTH_STENCIL_ATTACHMENT_OPTIMAL).
    if (depth < 0 || !hdrColor || in[depth].storeOp == VK_ATTACHMENT_STORE_OP_STORE || !g_evaluate) return false;
    out.assign(in, in + n);
    out[depth].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    static volatile LONG logged;
    if (InterlockedIncrement(&logged) <= 4) VRLOG("upint: scene pass depth storeOp -> STORE");
    return true;
}

bool patchPass(VkRenderPassCreateInfo& ci, std::vector<VkAttachmentDescription>& att) {
    if (!patchAttachments(ci.pAttachments, ci.attachmentCount, att)) return false;
    ci.pAttachments = att.data();
    return true;
}
bool patchPass2(VkRenderPassCreateInfo2& ci, std::vector<VkAttachmentDescription2>& att) {
    if (!patchAttachments(ci.pAttachments, ci.attachmentCount, att)) return false;
    ci.pAttachments = att.data();
    return true;
}

void onBeginPass(VkCommandBuffer cb) {
    std::lock_guard<std::mutex> l(g_passLock);
    g_openPass.insert(cb);
}
void onEndPass(VkCommandBuffer cb) {
    std::lock_guard<std::mutex> l(g_passLock);
    g_openPass.erase(cb);
}

void onImageBarrier(VkCommandBuffer cb, VkImage image, VkImageLayout from, VkImageLayout) {
    if (!t_ppDepth || image != g_color.image || cb != t_ppCb) return;
    if (t_pendingEye) {
        const int eye = t_pendingEye;
        t_pendingEye = 0;
        bool open;
        {
            std::lock_guard<std::mutex> l(g_passLock);
            open = g_openPass.count(cb) != 0;
        }
        if (from == VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL && !open) evaluateEye(cb, eye);
        else {
            static volatile LONG logged;
            if (InterlockedIncrement(&logged) <= 3) VRLOG("upint: eye %d not evaluated (canvas layout %d, pass open %d)", eye, int(from), int(open));
        }
        return;
    }
    // Learn which per-eye PP dispatch reads the canvas: its post-processing transitions it first.
    if (g_ppFlags != -1) return;
    if (from != VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL) {
        static volatile LONG warned;
        if (!InterlockedExchange(&warned, 1)) VRLOG("upint: canvas enters post-processing in layout %d, not evaluating", int(from));
        return;
    }
    InterlockedExchange(&g_ppFlags, LONG(t_ppFlags));
    VRLOG("upint: post-processing dispatch with flags 0x%x reads the canvas", t_ppFlags);
}

void onBeginCommandBuffer(VkCommandBuffer cb) {
    std::lock_guard<std::mutex> l(g_passLock);
    g_openPass.erase(cb);
}

core::Fov jitterProjection(int eye, const core::Fov& fov, float n, float f, int64_t frameTime, void* caller) {
    if (eye != 1 && eye != 2) return fov;
    if (g_trace > 0 && g_trace-- > 0) VRLOG("upint: trace projection eye %d frame %u time %lld from %p", eye, g_frame + (frameTime != g_frameTime), (long long)frameTime, caller);
    if (frameTime != g_frameTime) {
        g_frameTime = frameTime;
        ++g_frame;
        if (g_evaluate && g_jitterOn && up::active() != up::Kind::Off && g_color.w) up::jitter(g_frame, g_color.w, g_color.w, g_jx, g_jy);
        else g_jx = g_jy = 0;
    }
    ProjRec& p = g_proj[eye - 1];
    p.appliedX = g_jx, p.appliedY = g_jy, p.appliedFrame = g_frame;
    p.tan = core::tangents(fov);
    p.n = n, p.f = f;
    p.valid = true;
    if (!g_jx && !g_jy) return fov;
    return core::angles(core::jittered(p.tan, g_jx * g_jitterScale, g_jy * g_jitterScale, g_color.w, g_color.h));
}

bool install() {
    g_evaluate = se4::options::get("upscaler", 0) != 0;
    if (!g_evaluate) return false;
    g_jitterOn = se4::options::get("upscale_jitter", 1) != 0;
    g_evaluateOn = se4::options::get("upscale_evaluate", 1) != 0;
    g_lodBias = se4::options::get("upscale_lod_bias", -100) / 100.0f;
    g_jitterScale = se4::options::get("upscale_jitter_scale", 100) / 100.0f;
    g_motionOn = se4::options::get("upscale_mv", 1) != 0;
    g_reportScale = se4::options::get("upscale_report_scale", 100) / 100.0f;
    g_jitterLag = se4::options::get("upscale_jitter_lag", 0);
    g_jitterSign[0] = se4::options::get("upscale_jitter_x", 1) < 0 ? -1.0f : 1.0f;
    g_jitterSign[1] = se4::options::get("upscale_jitter_y", 1) < 0 ? -1.0f : 1.0f;
    const auto& b = se4::build();
    auto* hdr = b.at<void>(se4::gen::hdr_targets);
    auto* pp = b.at<void>(se4::gen::pp_dispatch);
    if (!hdr || !pp || !b.at<void>(se4::gen::vrd_head_world)) {
        g_evaluate = false;
        return false;
    }
    if (MH_CreateHook(hdr, reinterpret_cast<void*>(hookedHdr), reinterpret_cast<void**>(&realHdr)) != MH_OK ||
        MH_EnableHook(hdr) != MH_OK || MH_CreateHook(pp, reinterpret_cast<void*>(hookedPp), reinterpret_cast<void**>(&realPp)) != MH_OK ||
        MH_EnableHook(pp) != MH_OK) {
        VRLOG("upint: hooks FAILED");
        g_evaluate = false;
        return false;
    }
    VRLOG("upint: upscaler hooks on the HDR canvas and post-processing dispatch");
    if (g_evaluate) installLodBias();
    return true;
}
}  // namespace upint
