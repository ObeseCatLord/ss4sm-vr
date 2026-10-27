#include "up/integrate.hpp"
#include "core/flat_view.hpp"
#include "core/reproject.hpp"
#include "core/upscale.hpp"
#include "se4/build.hpp"
#include "se4/cvars.hpp"
#include "se4/foliage.hpp"
#include "se4/log.hpp"
#include "se4/options.hpp"
#include "se4/patch.hpp"
#include "se4/player_vr.hpp"
#include "se4/vr_device.hpp"
#include "up/upscaler.hpp"
#include "xr/runtime.hpp"
#include <windows.h>
#include <MinHook.h>
#include <cstdlib>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <unordered_map>
#include <unordered_set>

namespace upint {
namespace {
const uint32_t kMotionSpv[] =
#include "motion.comp.inc"
    ;
const uint32_t kRcasSpv[] =
#include "rcas.comp.inc"
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
thread_local int t_pendingSlot;  // slot + 1 to evaluate at the PP's first canvas barrier (0: none)
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
    PFN_vkCmdBlitImage blit;
    PFN_vkCmdClearColorImage clear;
    PFN_vkCreateBuffer createBuffer;
    PFN_vkGetBufferMemoryRequirements bufReq;
    PFN_vkBindBufferMemory bindBuf;
    PFN_vkMapMemory map;
    PFN_vkCmdCopyImageToBuffer copyToBuf;
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
    uint32_t ow = 0, oh = 0;  // output image size (phase A: the canvas; phase B: the eye drawport)
    VkImageView colorView = VK_NULL_HANDLE, depthView = VK_NULL_HANDLE;
    OwnImage motion, output;
    VkDescriptorSet set = VK_NULL_HANDLE;
    // RCAS sharpening (up::sharpness): output -> sharp, then sharp is copied to the canvas. Made at the output size.
    OwnImage sharp;
    uint32_t sw = 0, sh = 0;
    VkDescriptorSet sharpSet = VK_NULL_HANDLE;
    VkDescriptorSet debugSet = VK_NULL_HANDLE;  // lab debug view: motion -> sharp
    bool sharpFailed = false;
} g_res;

// Created once per device.
VkSampler g_sampler;
VkDescriptorSetLayout g_dsl;
VkPipelineLayout g_pl;
VkPipeline g_pipe;
VkDescriptorPool g_pool;
VkDescriptorSetLayout g_rcasDsl;
VkPipelineLayout g_rcasPl;
VkPipeline g_rcasPipe;
// The engine's depth partition (viewport depth range [split, 1] for the world, [0, split] beyond), from vkCmdSetViewport.
std::atomic<float> g_depthSplit{0.0f};
int g_passTrace = -1;  // option upscale_pass_trace (frames), read once
volatile LONG g_traceFrames, g_traceSeq;
int g_debugView;  // lab: upscale_debug_view 1 paints the view distance, 2 the motion vectors, instead of the image

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
    VkDescriptorPoolSize ps[2] = {{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 4}, {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 8}};
    VkDescriptorPoolCreateInfo dp{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    dp.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
    dp.maxSets = 4;
    dp.poolSizeCount = 2;
    dp.pPoolSizes = ps;
    return vk.createPool(vk.dev, &dp, nullptr, &g_pool) == VK_SUCCESS;
}

// The RCAS sharpening pipeline (src/up/shaders/rcas.comp): two storage images, 16 bytes of push constants. Made on
// first use; a failure only turns sharpening off.
bool createRcasPipeline() {
    if (g_rcasPipe) return true;
    VkDescriptorSetLayoutBinding b[2] = {};
    b[0] = {0, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
    b[1] = {1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
    VkDescriptorSetLayoutCreateInfo dl{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    dl.bindingCount = 2;
    dl.pBindings = b;
    if (!g_rcasDsl && vk.createDsl(vk.dev, &dl, nullptr, &g_rcasDsl) != VK_SUCCESS) return false;
    VkPushConstantRange pr{VK_SHADER_STAGE_COMPUTE_BIT, 0, 16};
    VkPipelineLayoutCreateInfo pl{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pl.setLayoutCount = 1;
    pl.pSetLayouts = &g_rcasDsl;
    pl.pushConstantRangeCount = 1;
    pl.pPushConstantRanges = &pr;
    if (!g_rcasPl && vk.createPl(vk.dev, &pl, nullptr, &g_rcasPl) != VK_SUCCESS) return false;
    VkShaderModuleCreateInfo sm{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    sm.codeSize = sizeof(kRcasSpv);
    sm.pCode = kRcasSpv;
    VkShaderModule mod;
    if (vk.createShader(vk.dev, &sm, nullptr, &mod) != VK_SUCCESS) return false;
    VkComputePipelineCreateInfo cp{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    cp.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_COMPUTE_BIT, mod, "main", nullptr};
    cp.layout = g_rcasPl;
    const VkResult r = vk.createCompute(vk.dev, VK_NULL_HANDLE, 1, &cp, nullptr, &g_rcasPipe);
    vk.destroyShader(vk.dev, mod, nullptr);
    return r == VK_SUCCESS;
}

// Called when the engine has re-created its HDR targets: the old canvas/depth are gone, so are any command buffers
// that used our views of them.
void releaseResources() {
    if (g_res.set) vk.freeSets(vk.dev, g_pool, 1, &g_res.set);
    if (g_res.colorView) vk.destroyView(vk.dev, g_res.colorView, nullptr);
    if (g_res.depthView) vk.destroyView(vk.dev, g_res.depthView, nullptr);
    destroyOwn(g_res.motion);
    destroyOwn(g_res.output);
    if (g_res.sharpSet) vk.freeSets(vk.dev, g_pool, 1, &g_res.sharpSet);
    if (g_res.debugSet) vk.freeSets(vk.dev, g_pool, 1, &g_res.debugSet);
    destroyOwn(g_res.sharp);
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
              createOwn(g_res.motion, g_res.w, g_res.h,
                        VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT) &&
              createOwn(g_res.output, g_res.w, g_res.h,
                        VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT);
    g_res.ow = g_res.w, g_res.oh = g_res.h;
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

// The output image at the output size. Phase B's eye drawport is normally the canvas size (device slot 0x068); it is
// re-created only when they differ, which happens when the eye size changes and the canvas is re-created as well.
bool ensureOutput(uint32_t w, uint32_t h) {
    if (!g_res.ready) return false;
    if (g_res.ow == w && g_res.oh == h) return g_res.output.image != VK_NULL_HANDLE;  // a failed size is not retried
    destroyOwn(g_res.output);
    if (g_res.sharpSet) vk.freeSets(vk.dev, g_pool, 1, &g_res.sharpSet), g_res.sharpSet = VK_NULL_HANDLE;
    if (g_res.debugSet) vk.freeSets(vk.dev, g_pool, 1, &g_res.debugSet), g_res.debugSet = VK_NULL_HANDLE;
    destroyOwn(g_res.sharp);
    g_res.sw = g_res.sh = 0;
    g_res.sharpFailed = false;
    g_res.ow = w, g_res.oh = h;
    const bool ok = createOwn(g_res.output, w, h,
                              VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT);
    VRLOG("upint: output image %ux%u %s", w, h, ok ? "ready" : "FAILED");
    if (!ok) destroyOwn(g_res.output);
    up::resetHistory();
    return ok;
}

// The sharpening target at the output image's size, with its descriptor set (output -> sharp). A failure is logged
// once per output size and leaves the image unsharpened.
bool ensureSharp() {
    if (g_res.sharp.image && g_res.sw == g_res.ow && g_res.sh == g_res.oh) return true;
    if (g_res.sharpFailed || !g_res.output.view) return false;
    bool ok = createRcasPipeline() && createOwn(g_res.sharp, g_res.ow, g_res.oh, VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
    VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    ai.descriptorPool = g_pool;
    ai.descriptorSetCount = 1;
    ai.pSetLayouts = &g_rcasDsl;
    if (ok) ok = vk.allocSets(vk.dev, &ai, &g_res.sharpSet) == VK_SUCCESS;
    if (ok) {
        VkDescriptorImageInfo in{VK_NULL_HANDLE, g_res.output.view, VK_IMAGE_LAYOUT_GENERAL};
        VkDescriptorImageInfo out{VK_NULL_HANDLE, g_res.sharp.view, VK_IMAGE_LAYOUT_GENERAL};
        VkWriteDescriptorSet w[2] = {};
        w[0] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, g_res.sharpSet, 0, 0, 1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, &in};
        w[1] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, g_res.sharpSet, 1, 0, 1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, &out};
        vk.updateSets(vk.dev, 2, w, 0, nullptr);
        g_res.sw = g_res.ow, g_res.sh = g_res.oh;
        if (g_debugView && vk.allocSets(vk.dev, &ai, &g_res.debugSet) == VK_SUCCESS) {
            VkDescriptorImageInfo mv{VK_NULL_HANDLE, g_res.motion.view, VK_IMAGE_LAYOUT_GENERAL};
            w[0].dstSet = w[1].dstSet = g_res.debugSet;
            w[0].pImageInfo = &mv;
            vk.updateSets(vk.dev, 2, w, 0, nullptr);
        }
    } else {
        if (g_res.sharpSet) vk.freeSets(vk.dev, g_pool, 1, &g_res.sharpSet), g_res.sharpSet = VK_NULL_HANDLE;
        destroyOwn(g_res.sharp);
        g_res.sharpFailed = true;
    }
    VRLOG("upint: sharpening image %ux%u %s", g_res.ow, g_res.oh, ok ? "ready" : "FAILED (no sharpening)");
    return ok;
}

// ---- per-view camera state ----
// Slots of the per-view arrays: 0 the flat (desktop) main view (docs/re/R27_flat_upscaler.md s5), 1 and 2 the VR eyes.
constexpr int kSlots = 3;
const char* slotName(int slot) { return slot == 0 ? "flat" : slot == 1 ? "eye 1" : "eye 2"; }
struct ProjRec {
    float appliedX = 0, appliedY = 0;  // jitter of the last projection handed to the engine for this eye
    uint32_t appliedFrame = 0;
    uint32_t rw = 0, rh = 0;  // render size the jitter was scaled for
    core::Tangents tan{};
    float n = 0.1f, f = 1000.0f;
    bool valid = false;
};
struct EyeHistory {
    bool valid = false;
    uint32_t frame = 0;
    core::Pose worldEye{};
    core::Tangents tan{};
    // Flat: the camera in the engine's doubles, the RenderView caller (player / camera entity) and the render size.
    double w2v[12] = {}, pos[3] = {};
    void* ret = nullptr;
    uint32_t w = 0, h = 0;
};
ProjRec g_proj[kSlots];
EyeHistory g_hist[kSlots];
uint32_t g_frame;
int64_t g_frameTime;
float g_jx, g_jy;
uint32_t g_jitW, g_jitH, g_jitOut;  // this frame's jitter sizes: render size (frustum shift) and output width (phases)
// Lab switches: the sign of the jitter reported to the upscaler (upscale_jitter_x/y), rendering without jitter
// (upscale_jitter=0), and jitter without the upscaler (upscale_evaluate=0).
float g_jitterSign[2] = {1, 1};
bool g_jitterOn = true;
bool g_evaluateOn = true;
float g_jitterScale = 1.0f;
bool g_motionOn = true;
float g_reportScale = 1.0f;
int g_jitterLag;  // lab: upscale_jitter_lag reports the jitter of frame (current - lag)  // lab: upscale_report_scale (percent) scales the jitter reported to the upscaler  // lab: upscale_mv=0 clears the motion vectors instead of computing them  // lab: upscale_jitter_scale (percent) magnifies the rendered jitter for visibility checks
LARGE_INTEGER g_lastEval[kSlots];

// Lab logging: the first 4 occurrences, then at most once a second.
struct LogRate {
    volatile LONG n = 0;
    volatile LONG64 last = 0;
};
bool logRate(LogRate& r) {
    if (r.n < 4 && InterlockedIncrement(&r.n) <= 4) return true;
    LARGE_INTEGER t, fq;
    QueryPerformanceCounter(&t);
    QueryPerformanceFrequency(&fq);
    const LONG64 last = r.last;
    return t.QuadPart - last >= fq.QuadPart && InterlockedCompareExchange64(&r.last, t.QuadPart, last) == last;
}

// ---- Phase B: the eye scene at out/ratio (docs/re/R25_upscale_render_size.md) ----
bool g_phaseBReady;  // option, symbols and opcode checks passed, hooks installed (installPhaseB)
// Desktop menu mode (docs/re/R28_rescaling_filter_menu.md s4) without upscale_quality in options.txt: the flat render
// size is the engine's own MPix-limiter result, and phase B is on while it is below the output (g_flatScaled, set by
// this frame's flat limiter call).
bool g_menuSize;
bool g_flatScaled;
float *g_maxMpix, *g_pixelRatio;  // ren_fMax3DRenderingMPix, ren_fRenderingPixelRatio (read only, logging)
// Menu mode: the backend the menu asked for (up::Kind as int, -1 none), applied at the next frame boundary (desktop: the
// player-views entry; VR: device slot 0x030, onVrFrameBegin).
std::atomic<int> g_request{-1};
// VR menu mode (docs/re/R29_upscaler_menu_vr_sharpening.md): the eye render size is a percentage of the eye target
// size per axis, chosen in the game's "Max 3D rendering Mpix" row (relabelled in VR); the user's MPix cap never applies
// to the eyes. Two-pass stereo while an upscaler runs or the percentage is below 100, single pass otherwise (unless
// option single_pass forces one), switched at the frame boundary.
bool g_vrMenu;
std::atomic<int> g_vrPct{100};
std::atomic<bool> g_vrDirty{true};
int g_singlePassForced = -1;  // option single_pass, -1 not set

// Render size of an eye target in VR: the menu percentage, else the upscale_quality ratio.
void vrRenderSize(uint32_t ew, uint32_t eh, uint32_t& rw, uint32_t& rh) {
    if (!g_vrMenu) return up::renderSize(ew, eh, rw, rh);
    const int p = std::clamp(g_vrPct.load(std::memory_order_relaxed), 1, 100);
    rw = std::max(1u, uint32_t(std::lround(double(ew) * p / 100.0)));
    rh = std::max(1u, uint32_t(std::lround(double(eh) * p / 100.0)));
}
// Armed by the RenderView limiter call of an eye's view: the render size it returned and the eye size it was given.
struct EyeSize {
    uint32_t rw = 0, rh = 0, ow = 0, oh = 0, frame = 0;
};
EyeSize g_size[kSlots];
// The post-processing dispatch of an eye in phase B (render thread): sizes, and whether the canvas got its
// output-size image (upscaled or blitted) at the PP's first canvas barrier.
struct PhaseB {
    int slot = -1;
    uint32_t rw = 0, rh = 0, ow = 0, oh = 0;
    bool filled = false;
};
thread_local PhaseB t_phaseB;

// ---- Flat (desktop) main view, slot 0 (docs/re/R27_flat_upscaler.md s3, s5) ----
// Desktop mode only (no VR device): RenderView of the main view inside the player-views pass is classified at entry,
// its MPix limiter call jitters gfx+0xc2c and reads the camera, its post-processing (by return address and dst) is
// upscaled as slot 0, and end_view restores the projection.
constexpr size_t kGfxProj = 0xc2c;       // P[16] floats RenderView copies into view+0x108 (rv_proj_copy)
constexpr size_t kGfxW2V = 0xc70;        // world-to-view 3x4 doubles (rv_view_copy)
constexpr size_t kGfxCamPos = 0xe48;     // camera position doubles (-R^T t)
constexpr size_t kGfxCurrentDp = 0x1c78; // current drawport pointer
constexpr size_t kViewDpCopy = 0x288;    // view command: copy of the current drawport, the PP command's dst
bool g_flatReady;          // desktop mode, symbols, opcodes and hooks verified (installFlat)
void** g_gfxGlobal;        // the engine gfx object (decoded at exec_eye_dp_load + 0x1c)
uint8_t* g_mainDp;         // the main flat view drawport (decoded at render_maindp_store)
void** g_backbufferTex;    // the backbuffer texture (decoded at gfx_backbuffer_get)
void** g_viewCmdGlobal;    // the view command RenderView builds (decoded at rv_viewcmd_store)
void *g_pvRet, *g_camRet;  // render_view return addresses: player view, camera entity (cutscenes)
void *g_ppCmdRet, *g_fmvRet;  // pp_dispatch return addresses: the per-view PP command, the video/image PP
int32_t *g_aaSamples, *g_maxRt, *g_forceCas;  // gfx_iAntiAliasingSamples, gfx_pixMaxRenderTargetSize, ren_bForceCAS (read only)
// The camera of this frame's flat main view, read at its limiter call before the jitter.
struct FlatCamera {
    double w2v[12] = {}, pos[3] = {};
    bool valid = false;
} g_flatCam;
// This frame's flat main view, recorded when its RenderView returns.
struct FlatView {
    void* view = nullptr;
    void* dst = nullptr;  // view + 0x288: the PP command's dst
    void* ret = nullptr;  // render_view return address (player or camera entity)
    uint32_t frame = 0;
} g_flat;
bool g_flatArmed;            // the current flat main view's limiter call jittered it and read its camera
volatile bool g_flatExec;    // from the flat main view's RenderView return to end_view (LOD bias)
float g_projSave[16], g_projJittered[16];  // gfx+0xc2c before / after the jitter, restored after end_view
bool g_projSaved;
thread_local int t_inViews, t_viewCount, t_rvDepth;  // inside player_views, its view count, render_view nesting
thread_local bool t_flatMain;                        // inside the flat main view's RenderView
thread_local int t_flatBoxW, t_flatBoxH;             // phase A: the HDR box of the flat post-processing

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

// Lab (upscale_debug_view): every ~2 s, the motion image's middle row (z = view distance) is copied to a host buffer
// and 16 samples of it are logged a frame later.
struct Readback {
    VkBuffer buf = VK_NULL_HANDLE;
    VkDeviceMemory mem = VK_NULL_HANDLE;
    uint16_t* data = nullptr;
    uint32_t w = 0;
    bool pending = false;
    ULONGLONG last = 0;
} g_rb;
float halfToFloat(uint16_t h) {
    const uint32_t s = (h >> 15) & 1, e = (h >> 10) & 0x1f, m = h & 0x3ff;
    float v = e == 0 ? std::ldexp(float(m), -24) : e == 31 ? INFINITY : std::ldexp(float(m | 0x400), int(e) - 25);
    return s ? -v : v;
}
void debugReadback(VkCommandBuffer cb, uint32_t W, uint32_t H) {
    if (g_rb.pending && g_rb.data) {
        for (int row = 0; row < 4; ++row) {
            char line[512];
            int n = 0;
            for (int i = 0; i < 16; ++i) {
                const float dist = halfToFloat(g_rb.data[(size_t(row) * g_rb.w + g_rb.w * (2 * i + 1) / 32) * 4 + 2]);
                n += snprintf(line + n, sizeof line - n, " %.1f", dist);
            }
            VRLOG("upint: debug distance row %d/8 (m):%s", row * 2 + 1, line);
        }
        g_rb.pending = false;
    }
    if (GetTickCount64() - g_rb.last < 2000) return;
    g_rb.last = GetTickCount64();
    if (!g_rb.buf || g_rb.w != W) {
        if (g_rb.buf) return;  // one size only (lab)
        VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        bi.size = VkDeviceSize(W) * 8 * 4;
        bi.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        if (vk.createBuffer(vk.dev, &bi, nullptr, &g_rb.buf) != VK_SUCCESS) return;
        VkMemoryRequirements mr;
        vk.bufReq(vk.dev, g_rb.buf, &mr);
        uint32_t type = UINT32_MAX;
        for (uint32_t i = 0; i < vk.mem.memoryTypeCount; ++i)
            if ((mr.memoryTypeBits & (1u << i)) && (vk.mem.memoryTypes[i].propertyFlags & (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) ==
                                                         (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) {
                type = i;
                break;
            }
        VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        ai.allocationSize = mr.size;
        ai.memoryTypeIndex = type;
        if (type == UINT32_MAX || vk.alloc(vk.dev, &ai, nullptr, &g_rb.mem) != VK_SUCCESS) return;
        vk.bindBuf(vk.dev, g_rb.buf, g_rb.mem, 0);
        void* p = nullptr;
        vk.map(vk.dev, g_rb.mem, 0, VK_WHOLE_SIZE, 0, &p);
        g_rb.data = static_cast<uint16_t*>(p);
        g_rb.w = W;
    }
    VkImageMemoryBarrier b;
    imageBarrier(b, g_res.motion.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                 VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_TRANSFER_READ_BIT);
    vk.barrier(cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);
    VkBufferImageCopy c[4] = {};
    for (int row = 0; row < 4; ++row) {  // rows at 1/8, 3/8, 5/8, 7/8 of the height
        c[row].bufferOffset = VkDeviceSize(row) * W * 8;
        c[row].imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        c[row].imageOffset = {0, int32_t(H * (2 * row + 1) / 8), 0};
        c[row].imageExtent = {W, 1, 1};
    }
    vk.copyToBuf(cb, g_res.motion.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, g_rb.buf, 4, c);
    imageBarrier(b, g_res.motion.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                 VK_ACCESS_TRANSFER_READ_BIT, VK_ACCESS_SHADER_READ_BIT);
    vk.barrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);
    g_rb.pending = true;
}

// Slot 0 is the flat main view (camera from the engine's matrices, sizes from the HDR box), 1/2 the VR eyes.
void evaluateEye(VkCommandBuffer cb, int slot) {
    const int e = slot;
    const bool flat = slot == 0;
    PhaseB& pb = t_phaseB;
    const bool scaled = pb.slot == slot;  // phase B: render pb.rw x pb.rh, output pb.ow x pb.oh
    if (g_passTrace > 0 && g_traceFrames <= g_passTrace) VRLOG("passtrace: frame %ld evaluate %s", InterlockedIncrement(&g_traceFrames) - 1, slotName(slot));
    if (!ensureResources() || (!scaled && !g_proj[e].valid)) return;
    core::Pose cur;
    const bool posed = g_proj[e].valid && (flat ? g_flatCam.valid : worldEye(slot, cur));
    if (!posed) {
        g_hist[e].valid = false;
        if (!scaled) return;  // phase B still fills the canvas at the output size (blit below)
    }
    // Rendered sub-rect at (0,0): VR phase A the canvas; flat phase A the HDR box, which is smaller than the canvas
    // with a letterbox or the view-size global (R27 s4.1).
    const uint32_t W = scaled ? pb.rw : flat ? uint32_t(t_flatBoxW) : g_res.w, H = scaled ? pb.rh : flat ? uint32_t(t_flatBoxH) : g_res.h;
    const uint32_t OW = scaled ? pb.ow : W, OH = scaled ? pb.oh : H;
    if (!W || !H || !ensureOutput(OW, OH)) return;
    EyeHistory& h = g_hist[e];
    bool reset = !h.valid || h.frame + 1 != g_frame;
    if (flat && (h.ret != g_flat.ret || h.w != W || h.h != H)) reset = true;  // player <-> camera entity, box change
    const float ws = flat ? 1.0f : worldScale();  // engine units per metre
    core::Pose c2p{};
    if (posed && !reset) {
        if (flat) reset = !core::currentToPreviousW2V(h.w2v, h.pos, g_flatCam.w2v, g_flatCam.pos, c2p);
        else c2p = core::currentToPrevious(h.worldEye, cur);
        const float moved = std::sqrt(core::dot(c2p.p, c2p.p)) * ws;  // metres
        const float turned = 2 * std::acos(std::min(1.0f, std::fabs(c2p.q.w)));
        if (moved > 1.0f || turned > 0.6f) reset = true;  // teleport / respawn / camera cut
    }
    if (reset) c2p = {};
    if (posed) {
        // Lab diagnostics: camera motion per frame and history resets, once a second per view.
        static float maxMove[kSlots], maxTurn[kSlots];
        static int resets[kSlots], evals[kSlots];
        static LARGE_INTEGER last[kSlots];
        const float moved = std::sqrt(core::dot(c2p.p, c2p.p)) * ws * 1000.0f;  // mm
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
                VRLOG("upint: %s %d evaluations, %d resets, camera motion max %.2f mm %.3f deg per frame", slotName(slot), evals[e], resets[e],
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
    // (compute and transfer too: the other eye's RCAS and copy read the shared motion / output / sharp images)
    vk.barrier(cb,
               VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT | VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT |
                   VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT,
               VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 4, b);

    const core::Tangents& ct = g_proj[e].tan;
    bool ok = false;
    if (posed) {
        // 2. camera motion vectors over the rendered sub-rect.
        struct {
            float curToPrev[16];
            float cur[4], prev[4], size[4], origin[4];
        } pc{};
        core::rigidMatrix(c2p, pc.curToPrev);
        const core::Tangents& pt = reset ? ct : h.tan;
        memcpy(pc.cur, &ct, sizeof pc.cur);
        memcpy(pc.prev, &pt, sizeof pc.prev);
        pc.size[0] = float(W), pc.size[1] = float(H), pc.size[2] = g_proj[e].n, pc.size[3] = g_proj[e].f;
        pc.origin[2] = g_depthSplit.load(std::memory_order_relaxed);
        pc.origin[3] = flat ? 0.25f : 0.0f;  // metres; flat only: VR hands and weapons are tracked, not camera-attached
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
        vk.barrier(cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr,
                   0, nullptr, 1, b);
        if (g_debugView) debugReadback(cb, W, H);

        // 3. the upscaler: phase A native AA (render size == output size == canvas), phase B the render sub-rect of the
        //    canvas images to the output size.
        LARGE_INTEGER now, freq;
        QueryPerformanceCounter(&now);
        QueryPerformanceFrequency(&freq);
        float ms = g_lastEval[e].QuadPart ? float(double(now.QuadPart - g_lastEval[e].QuadPart) * 1000.0 / double(freq.QuadPart)) : 11.1f;
        g_lastEval[e] = now;
        up::Frame f;
        f.cmd = cb;
        f.color = {g_res.color, g_res.colorView, g_color.format, g_res.w, g_res.h};
        f.depth = {g_res.depth, g_res.depthView, g_depth.format, g_res.w, g_res.h};
        f.motion = {g_res.motion.image, g_res.motion.view, VK_FORMAT_R16G16B16A16_SFLOAT, g_res.w, g_res.h};
        f.output = {g_res.output.image, g_res.output.view, VK_FORMAT_R16G16B16A16_SFLOAT, OW, OH};
        f.renderW = W, f.renderH = H;
        float rx = g_jx, ry = g_jy;
        if (g_jitterLag) up::jitter(uint32_t(int(g_frame) - g_jitterLag), g_jitW, g_jitOut, rx, ry);  // lab: report another frame's jitter
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
        ok = up::evaluate(slot, f);
    }

    // 3b. RCAS sharpening of the upscaled image (up::sharpness, 0 = off): output -> sharp, which is then copied.
    VkImage result = g_res.output.image;
    const float sharpness = up::sharpness();
    const bool debug = ok && g_debugView && g_res.debugSet;
    if (ok && (sharpness > 0.0f || g_debugView) && ensureSharp()) {
        imageBarrier(b[0], g_res.output.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL,
                     VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT);
        imageBarrier(b[1], g_res.sharp.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL, 0,
                     VK_ACCESS_SHADER_WRITE_BIT);
        vk.barrier(cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr,
                   0, nullptr, 2, b);
        if (debug) {  // the motion image is sampled by the upscaler (read-only layout): storage reads need GENERAL
            imageBarrier(b[0], g_res.motion.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL,
                         VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_SHADER_READ_BIT);
            vk.barrier(cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1, b);
        }
        const float pc[4] = {sharpness, float(OW), float(OH), debug ? float(g_debugView) : 0.0f};
        vk.bindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, g_rcasPipe);
        vk.bindSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, g_rcasPl, 0, 1, debug ? &g_res.debugSet : &g_res.sharpSet, 0, nullptr);
        vk.push(cb, g_rcasPl, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof pc, pc);
        vk.dispatch(cb, (OW + 7) / 8, (OH + 7) / 8, 1);
        result = g_res.sharp.image;
        static float logged = -1.0f;
        if (logged != sharpness) VRLOG("upint: RCAS sharpening %.0f%% (%s)", sharpness * 100.0f, up::name()), logged = sharpness;
    }

    // 4. copy back into the canvas at (0,0) and restore the engine's layouts. Phase B without an upscaled image blits
    //    the render sub-rect to the output size first (bilinear; R25 s5 step 7), so post-processing reads a full image.
    VkPipelineStageFlags restoreSrc = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
    uint32_t n = 0;
    const bool blit = scaled && !ok;
    if (blit) {
        imageBarrier(b[0], g_res.color, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                     VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_TRANSFER_READ_BIT);
        imageBarrier(b[1], g_res.output.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                     VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
        vk.barrier(cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0,
                   nullptr, 2, b);
        VkImageBlit bl{};
        bl.srcSubresource = bl.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        bl.srcOffsets[1] = {int32_t(W), int32_t(H), 1};
        bl.dstOffsets[1] = {int32_t(OW), int32_t(OH), 1};
        vk.blit(cb, g_res.color, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, g_res.output.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &bl,
                VK_FILTER_LINEAR);
        imageBarrier(b[0], g_res.output.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                     VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT);
        imageBarrier(b[1], g_res.color, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                     VK_ACCESS_TRANSFER_READ_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
        vk.barrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 2, b);
        static LogRate rate;
        if (logRate(rate))
            VRLOG("upint: %s phase B blit fallback %ux%u -> %ux%u (%s)", slotName(slot), W, H, OW, OH, posed ? "upscaler failed" : "no eye pose");
    } else if (ok) {
        imageBarrier(b[0], result, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                     VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT);
        imageBarrier(b[1], g_res.color, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                     VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
        vk.barrier(cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 2, b);
    }
    if (ok || blit) {
        VkImageCopy rc{};
        rc.srcSubresource = rc.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        rc.extent = {OW, OH, 1};
        vk.copy(cb, blit ? g_res.output.image : result, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, g_res.color, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1,
                &rc);
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
    if (scaled) pb.filled = ok || blit;

    if (!posed) return;
    h.valid = ok;
    h.frame = g_frame;
    h.worldEye = cur;
    h.tan = ct;
    if (flat) {
        memcpy(h.w2v, g_flatCam.w2v, sizeof h.w2v);
        memcpy(h.pos, g_flatCam.pos, sizeof h.pos);
        h.ret = g_flat.ret;
        h.w = W, h.h = H;
    }
    if (g_trace > 0) VRLOG("upint: trace evaluate %s frame %u jitter %.3f %.3f", slotName(slot), g_frame, g_jx, g_jy);
    static volatile LONG logged;
    if (InterlockedIncrement(&logged) <= 4) {
        if (scaled)
            VRLOG("upint: %s %s %ux%u -> %ux%u (phase B, frame %u, jitter %.3f %.3f, reset %d)", slotName(slot), ok ? "evaluated" : "evaluate FAILED",
                  W, H, OW, OH, g_frame, g_jx, g_jy, int(reset));
        else
            VRLOG("upint: %s %s %ux%u (frame %u, jitter %.3f %.3f, reset %d)", slotName(slot), ok ? "evaluated" : "evaluate FAILED", W, H, g_frame,
                  g_jx, g_jy, int(reset));
    }
}

// ---- engine hooks ----
VkCommandBuffer engineCommandBuffer(void* gfx) {
    if (!gfx) return VK_NULL_HANDLE;
    auto cb = *reinterpret_cast<VkCommandBuffer*>(static_cast<uint8_t*>(gfx) + kGfxCommandBuffer);
    if (!cb || (*reinterpret_cast<uintptr_t*>(cb) & 0xffffffffu) != kLoaderMagic) return VK_NULL_HANDLE;
    return cb;
}

// The eye the engine renders now (device +0x18: 1 left, 2 right, 3 scope), -1 without a device.
int deviceEye() {
    void** devp = se4::build().at<void*>(se4::gen::vr_device_global);
    return devp && *devp ? *reinterpret_cast<int32_t*>(static_cast<uint8_t*>(*devp) + 0x18) : -1;
}

uint8_t* gfxObject() { return g_gfxGlobal ? static_cast<uint8_t*>(*g_gfxGlobal) : nullptr; }

// The flat main view is upscaled now (R27 s5 step 9): desktop mode with verified symbols, an upscaler running, and
// no MSAA (the MSAA scene depth is not single-sample and the canvas is resolved inside the PP).
bool flatEnabled() {
    if (!g_flatReady) return false;
    const int aa = g_aaSamples ? *g_aaSamples : 1;
    const char* why = nullptr;
    if (!g_evaluate) why = "upscaler disabled";
    else if (up::active() == up::Kind::Off) why = up::menuMode() ? "upscaler off (Rescaling Filter menu)" : "no upscaler backend";
    else if (aa > 1) why = "MSAA";
    // Logged once per change of state.
    static PVOID volatile logged;
    PVOID state = const_cast<char*>(why ? why : "on");
    if (InterlockedExchangePointer(&logged, state) != state) {
        if (!why) VRLOG("upint: flat on: %s, phase %s", up::name(), phaseB() ? "B" : "A");
        else if (aa > 1) VRLOG("upint: flat off: MSAA x%d (gfx_iAntiAliasingSamples must be 1; the game renders stock)", aa);
        else VRLOG("upint: flat off: %s", why);
    }
    return !why;
}

// ---- Texture LOD bias while upscaling (docs/re/R23_lod_bias.md) ----
// A temporal upscaler needs a negative mip bias (log2(render/display) - 1: -1 for DLAA / FSR native AA) or fine
// texture detail washes out. The engine's two scene texture binds load tex_fLODBias into the sampler state; both loads
// jump to stubs that add g_extraBias (the cvar itself is never written). g_extraBias is set only while an eye's scene
// executes with the upscaler evaluating, and cleared before its post-processing. Phase B adds log2(render/output).
volatile float* g_extraBias;
float g_lodBias = -1.0f;  // option upscale_lod_bias (hundredths), default -100
using ExecFn = void(__cdecl*)(void* a1, void* a2, void* a3, void* a4);
ExecFn realExecute;
bool upscaling() { return g_evaluate && g_evaluateOn && g_jitterOn && up::active() != up::Kind::Off; }
thread_local bool t_ppWroteEye;  // phase B: this view's post-processing wrote the eye drawport (skip the rescale)
// The RenderView limiter call of this slot's view in this frame shrank its render size.
bool armed(int slot) { return g_size[slot].rw && g_size[slot].frame == g_frame; }
void __cdecl hookedExecute(void* a1, void* a2, void* a3, void* a4) {
    const int eye = deviceEye();
    // VR eyes 1/2; the flat main view (slot 0) between its RenderView return and end_view.
    const int slot = (eye == 1 || eye == 2) ? eye : (eye == -1 && g_flatExec) ? 0 : -1;
    t_ppWroteEye = false;
    if (g_extraBias) {
        float bias = 0.0f;
        if (slot >= 0 && upscaling()) {
            bias = g_lodBias;
            if (phaseB() && armed(slot)) bias += up::mipBias(g_size[slot].rw, g_size[slot].ow);
        }
        *g_extraBias = bias;
    }
    realExecute(a1, a2, a3, a4);
    if (g_extraBias) *g_extraBias = 0.0f;
    t_ppWroteEye = false;
}
// The two bind patches (R23 s4.1).
bool patchLodBias() {
    const auto& b = se4::build();
    auto* tex = b.at<uint8_t>(se4::gen::tex_fLODBias);
    if (!tex) return false;
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
    VRLOG("upint: texture LOD bias %+.2f while upscaling%s (binds %p, %p)", g_lodBias, g_phaseBReady ? " (+ log2(render/output) in phase B)" : "",
          static_cast<void*>(site[0]), static_cast<void*>(site[1]));
    return true;
}
// Phase B needs the execute-view hook (it ends the rescale skip) and the bias even when upscale_lod_bias is 0.
bool installLodBias() {
    void* exec = se4::build().at<void>(se4::gen::execute_view);
    if (!exec || (g_lodBias == 0.0f && !g_phaseBReady)) return false;
    if (!patchLodBias() && !g_phaseBReady) return false;
    if (MH_CreateHook(exec, reinterpret_cast<void*>(hookedExecute), reinterpret_cast<void**>(&realExecute)) != MH_OK || MH_EnableHook(exec) != MH_OK) {
        if (g_phaseBReady) VRLOG("upint: phase B off: execute-view hook FAILED");
        g_phaseBReady = false;
        if (g_extraBias) *g_extraBias = 0.0f;
        return VRLOG("upint: execute-view hook FAILED - LOD bias stays 0"), false;
    }
    return true;
}

// ---- Phase B hooks (docs/re/R25_upscale_render_size.md s1.3, s3.2) ----
using LimiterFn = float(__cdecl*)(int w, int h, int* outW, int* outH);  // returns ren_fRenderingPixelRatio in xmm0
// rescale_pass and gfx_copy are typed only as far as the R25 call sites show them: the integer arguments go through as
// full registers/slots and the return register is passed back.
using RescaleFn = uintptr_t(__cdecl*)(void* src, void* dst, uintptr_t mode);
using CopyFn = uintptr_t(__cdecl*)(void* gfx, void* src, uintptr_t a, void* dst, uintptr_t b, uintptr_t filtered);
using HdrSubrectFn = uint8_t*(__cdecl*)(int singleSample, int w, int h);
LimiterFn realLimiter;
RescaleFn realRescale;
CopyFn realCopy;
HdrSubrectFn hdrSubrect;
void* g_rvRet;           // rv_limiter_call + 5: RenderView sizes its 3D canvas
void* g_hdrRet;          // hdr_limiter_call + 5: the HDR canvas allocation's upper bound
uint8_t* g_canvas3dDp;   // the 3D canvas drawport (decoded at exec_rescale_site)
void** g_eyeDpGlobal;    // the eye drawport RenderView saved (decoded at exec_eye_dp_load)
uint8_t* g_hdrCanvasDp;  // hdr_canvas
volatile LONG g_skips, g_eyePp;  // rescales skipped, phase-B post-processing dispatches that wrote the eye drawport

// A drawport's box size (+0x10 x0, +0x14 y0, +0x18 x1, +0x1c y1; R25 s2.1).
void boxSize(const void* dp, int& w, int& h) {
    const int32_t* b = reinterpret_cast<const int32_t*>(static_cast<const uint8_t*>(dp) + 0x10);
    w = b[2] - b[0], h = b[3] - b[1];
}
bool boxAtOrigin(const void* dp) {
    const int32_t* b = reinterpret_cast<const int32_t*>(static_cast<const uint8_t*>(dp) + 0x10);
    return b[0] == 0 && b[1] == 0;
}

// ---- Flat main view hooks (docs/re/R27_flat_upscaler.md s3.2, s5 steps 3-5) ----
// player_views, render_view and end_view take at most four integer/pointer arguments (prologues read rcx, rdx, r8,
// r9 only): they go through as full registers and the return register is passed back.
using PassFn = uintptr_t(__cdecl*)(uintptr_t a1, uintptr_t a2, uintptr_t a3, uintptr_t a4);
PassFn realPlayerViews, realRenderView, realEndView;

const char* rvKind(const void* ret) { return ret == g_pvRet ? "player" : ret == g_camRet ? "camera" : "other"; }

// A backend switch from the menu, between two frames: no view of this frame has been limited, jittered or post-processed
// yet, so nothing of the old backend's frame is left half done. The flat history starts over.
void applyRequest() {
    const int k = g_request.exchange(-1);
    if (k < 0) return;
    const up::Kind before = up::active();
    if (!up::select(up::Kind(k))) {
        VRLOG("upint: flat upscaler %s requested but not available - stays %s", up::name(up::Kind(k)), up::name());
        return;
    }
    if (up::active() == before) return;
    g_hist[0] = {};
    g_proj[0].valid = false;
    g_lastEval[0] = {};
    g_flatArmed = false;
    VRLOG("upint: flat upscaler %s -> %s at a frame boundary", up::name(before), up::name());
}

// VR menu mode, at device slot 0x030 (the start of a VR frame, before any eye is projected or rendered): the menu's
// backend switch and the stereo path. Single pass (about 15 % less game CPU, docs/PERF.md) only runs without an upscaler
// at 100 %: the upscalers and the render-size override need the per-eye views of two-pass stereo.
void applyVrRequest() {
    const int k = g_request.exchange(-1);
    const bool dirty = g_vrDirty.exchange(false);
    if (k < 0 && !dirty) return;
    if (k >= 0) {
        const up::Kind before = up::active();
        if (!up::select(up::Kind(k))) VRLOG("upint: VR upscaler %s requested but not available - stays %s", up::name(up::Kind(k)), up::name());
        else if (up::active() != before) {
            for (int e = 1; e <= 2; ++e) g_hist[e] = {}, g_proj[e].valid = false, g_lastEval[e] = {};
            VRLOG("upint: VR upscaler %s -> %s at a frame boundary", up::name(before), up::name());
        }
    }
    if (g_singlePassForced >= 0) return;
    const bool single = up::active() == up::Kind::Off && g_vrPct.load() >= 100;
    auto* cur = se4::build().at<int32_t>(se4::gen::vr_bSinglePrepareRendering);
    if (cur && (*cur != 0) != single) {
        se4::setIndex(se4::gen::vr_bSinglePrepareRendering, single ? 1 : 0);
        VRLOG("upint: VR stereo %s (%s, %d%%)", single ? "single pass" : "two-pass", up::name(), g_vrPct.load());
    }
}

// player_views(dp, pi+0xa8, views, size): the per-frame pass over the local views; views+0xc = their count.
uintptr_t __cdecl hookedPlayerViews(uintptr_t dp, uintptr_t p2, uintptr_t views, uintptr_t size) {
    if (!t_inViews) {
        applyRequest();
        foliage::update(flatEnabled());
    }
    const int savedCount = t_viewCount;
    t_viewCount = views ? *reinterpret_cast<const int32_t*>(views + 0xc) : 0;
    ++t_inViews;
    const uintptr_t r = realPlayerViews(dp, p2, views, size);
    --t_inViews;
    t_viewCount = savedCount;
    return r;
}

// Lab logging: the main drawport, its texture and the backbuffer texture (width/height at +0x20/+0x24), on change.
void logMainDp() {
    static void *lastTex, *lastBb;
    static int lastW = -1, lastH = -1, lastViews = -1;
    void* tex = *reinterpret_cast<void**>(g_mainDp);
    void* bb = *g_backbufferTex;
    int bw, bh;
    boxSize(g_mainDp, bw, bh);
    if (tex == lastTex && bb == lastBb && bw == lastW && bh == lastH && t_viewCount == lastViews) return;
    lastTex = tex, lastBb = bb, lastW = bw, lastH = bh, lastViews = t_viewCount;
    const uint32_t* bs = bb ? reinterpret_cast<const uint32_t*>(static_cast<uint8_t*>(bb) + 0x20) : nullptr;
    VRLOG("upint: flat: main dp %p tex %p box %dx%d backbuffer %ux%u views %d%s", static_cast<void*>(g_mainDp), tex, bw, bh, bs ? bs[0] : 0u,
          bs ? bs[1] : 0u, t_viewCount, tex == bb ? "" : " (main dp texture is not the backbuffer texture)");
}

// RenderView (R27 s3.2): the flat main view is the one inside the player-views pass without split screen whose
// current drawport is on the main drawport's texture (the main dp, its view-size shrink or a camera letterbox).
uintptr_t __cdecl hookedRenderView(uintptr_t a1, uintptr_t a2, uintptr_t a3, uintptr_t a4) {
    void* ret = __builtin_return_address(0);  // MinHook enters by a jmp: the engine caller's return address
    bool mainView = false;
    void* cur = nullptr;
    int cw = 0, ch = 0;
    if (t_inViews && !t_rvDepth && deviceEye() == -1 && flatEnabled()) {
        uint8_t* gfx = gfxObject();
        cur = gfx ? *reinterpret_cast<void**>(gfx + kGfxCurrentDp) : nullptr;
        void* tex = cur ? *static_cast<void**>(cur) : nullptr;
        mainView = t_viewCount == 1 && tex && tex == *reinterpret_cast<void**>(g_mainDp);
        if (cur) boxSize(cur, cw, ch);
        logMainDp();
    }
    if (mainView) g_flatArmed = false;
    const bool saved = t_flatMain;
    t_flatMain = mainView;
    ++t_rvDepth;
    const uintptr_t r = realRenderView(a1, a2, a3, a4);
    --t_rvDepth;
    t_flatMain = saved;
    if (mainView) {
        void* view = g_flatArmed ? *g_viewCmdGlobal : nullptr;
        if (view) {
            if (g_flat.ret != ret && g_flat.ret)
                VRLOG("upint: flat rv: caller %s -> %s (history reset)", rvKind(g_flat.ret), rvKind(ret));
            g_flat = {view, static_cast<uint8_t*>(view) + kViewDpCopy, ret, g_frame};
            if (g_debugView) {  // lab: the scene's projection (view+0x108) and the API copy (gfx+0xdf0) against the one the mod reads
                static LogRate prate;
                const float* V = reinterpret_cast<const float*>(static_cast<uint8_t*>(view) + 0x108);
                uint8_t* gfx = gfxObject();
                const float* A = gfx ? reinterpret_cast<const float*>(gfx + 0xdf0) : nullptr;
                if (logRate(prate))
                    VRLOG("upint: debug P view+0x108 [10] %.6f [11] %.6f [14] %.3f [15] %.3f | gfx+0xdf0 [10] %.6f [11] %.6f [14] %.3f | mod n %.4f f %.1f",
                          V[10], V[11], V[14], V[15], A ? A[10] : 0.f, A ? A[11] : 0.f, A ? A[14] : 0.f, g_proj[0].n, g_proj[0].f);
            }
            g_flatExec = true;
        } else g_flatArmed = false;
        static LogRate rate;
        if (logRate(rate))
            VRLOG("upint: flat rv: %s cur %p box %dx%d%s", rvKind(ret), cur, cw, ch, g_flatArmed ? "" : " (not armed: stock view)");
    }
    return r;
}

// end_view pops the drawport stack, which writes back the gfx+0xc2c that RenderView's dp_push saved (the jittered
// one), so the unjittered projection is restored only after it (R27 s1.5). It is restored only while gfx+0xc2c still
// holds the matrix the jitter wrote; anything newer is the engine's.
uintptr_t __cdecl hookedEndView(uintptr_t a1, uintptr_t a2, uintptr_t a3, uintptr_t a4) {
    const uintptr_t r = realEndView(a1, a2, a3, a4);
    uint8_t* gfx = gfxObject();
    if (g_projSaved && !t_rvDepth && gfx) {
        float* P = reinterpret_cast<float*>(gfx + kGfxProj);
        if (!memcmp(P, g_projJittered, sizeof g_projJittered)) {
            memcpy(P, g_projSave, sizeof g_projSave);
            g_projSaved = false;
        } else {
            static LogRate rate;
            if (logRate(rate)) VRLOG("upint: flat: end_view: gfx+0xc2c changed since the jitter, not restored here");
        }
    }
    g_flatExec = false;
    return r;
}

// The flat main view's limiter call inside RenderView (R27 s5 step 4): render size, then camera and jitter.
void flatLimiter(int w, int h, int* ow, int* oh) {
    uint8_t* gfx = gfxObject();
    void* cur = gfx ? *reinterpret_cast<void**>(gfx + kGfxCurrentDp) : nullptr;
    if (!cur) return;
    float* P = reinterpret_cast<float*>(gfx + kGfxProj);
    static LogRate rate, prate;
    if (!core::isPerspective(P)) {
        // Orthographic camera: the engine's own render size and no jitter (stock view).
        if (logRate(prate)) VRLOG("upint: flat limiter: P[14] %.3f is not a perspective projection - stock view", P[14]);
        return;
    }
    if (g_projSaved) {
        static LogRate missed;
        if (logRate(missed)) VRLOG("upint: flat: the previous projection was not restored at end_view (dropped)");
        g_projSaved = false;
    }
    const int engineW = *ow, engineH = *oh;
    uint32_t tw = uint32_t(w), th = uint32_t(h);
    if (g_menuSize) {
        // Desktop menu mode (R28 s4): the engine's own size for this output ("Max 3D rendering Mpix" x pixel ratio, with
        // its clamps and 0.95 dead-band), at least a third of the output per axis. Below the output: phase B (the
        // engine's rescale is skipped, the upscaler fills the output); equal: phase A (native AA).
        core::menuRenderSize(uint32_t(w), uint32_t(h), engineW, engineH, tw, th);
        g_flatScaled = tw < uint32_t(w) || th < uint32_t(h);
        if (!phaseB()) tw = uint32_t(w), th = uint32_t(h);  // phase B unavailable: native AA at the output size
        static int lastW = -1, lastH = -1, lastKind = -1;
        static LogRate mrate;
        if (int(tw) != lastW || int(th) != lastH || int(up::active()) != lastKind || logRate(mrate)) {
            lastW = int(tw), lastH = int(th), lastKind = int(up::active());
            VRLOG("upint: flat limiter (menu %s): out %dx%d -> engine %dx%d (MPix %.2f ratio %.3f) -> render %ux%u (phase %s)", up::name(), w, h, engineW,
                  engineH, g_maxMpix ? *g_maxMpix : -1.0f, g_pixelRatio ? *g_pixelRatio : -1.0f, tw, th, tw < uint32_t(w) || th < uint32_t(h) ? "B" : "A");
        }
    } else if (phaseB()) up::renderSize(uint32_t(w), uint32_t(h), tw, th);  // phase B: out / ratio
    *ow = int(tw), *oh = int(th);  // phase A: the identity (the user's pixel ratio and MPix limit are bypassed)
    int bw, bh, rbw, rbh;
    boxSize(cur, bw, bh);
    core::flatRenderBox(w, h, *ow, *oh, bw, bh, g_forceCas && *g_forceCas, g_maxRt ? *g_maxRt : 0, rbw, rbh);
    if (bw <= 0 || bh <= 0 || rbw <= 0 || rbh <= 0) {
        if (logRate(prate)) VRLOG("upint: flat limiter: empty box %dx%d / render box %dx%d - stock view", bw, bh, rbw, rbh);
        *ow = engineW, *oh = engineH;
        return;
    }
    if (logRate(rate)) VRLOG("upint: flat limiter: tex %dx%d -> %dx%d render box %dx%d out %dx%d (engine %dx%d)", w, h, *ow, *oh, rbw, rbh, bw, bh, engineW, engineH);

    // Camera (R27 s2), read before the jitter.
    ProjRec& p = g_proj[0];
    p.tan = core::projectionTangents(P);
    const bool nf = core::projectionNearFar(P, p.n, p.f);
    p.rw = uint32_t(rbw), p.rh = uint32_t(rbh);
    p.valid = nf && std::isfinite(p.tan.l) && std::isfinite(p.tan.r) && std::isfinite(p.tan.u) && std::isfinite(p.tan.d);
    memcpy(g_flatCam.w2v, gfx + kGfxW2V, sizeof g_flatCam.w2v);
    double e48[3], rt[3];
    memcpy(e48, gfx + kGfxCamPos, sizeof e48);
    core::viewPosition(g_flatCam.w2v, rt);
    const double diff = std::sqrt((e48[0] - rt[0]) * (e48[0] - rt[0]) + (e48[1] - rt[1]) * (e48[1] - rt[1]) + (e48[2] - rt[2]) * (e48[2] - rt[2]));
    // The engine's double camera position when it matches -R^T t (it does for the player, camera entity and precache
    // views); otherwise the one derived from the matrix.
    memcpy(g_flatCam.pos, diff < 0.01 ? e48 : rt, sizeof g_flatCam.pos);
    core::Pose pose;
    g_flatCam.valid = core::viewPoseFromW2V(g_flatCam.w2v, pose) && std::isfinite(diff);
    static LogRate poseRate;
    if (logRate(poseRate))
        VRLOG("upint: flat pose: pos %.2f %.2f %.2f (e48 vs -Rt diff %.4g) tan %.4f %.4f %.4f %.4f n %.3f f %.0f%s", g_flatCam.pos[0], g_flatCam.pos[1],
              g_flatCam.pos[2], diff, p.tan.l, p.tan.r, p.tan.u, p.tan.d, p.n, p.f, g_flatCam.valid && p.valid ? "" : " (INVALID)");

    // Jitter (R27 s1.5): one Halton step per flat main view, scaled for the render box; restored after end_view.
    ++g_frame;
    g_jitW = uint32_t(rbw), g_jitH = uint32_t(rbh), g_jitOut = uint32_t(bw);
    if (g_jitterOn) up::jitter(g_frame, g_jitW, g_jitOut, g_jx, g_jy);
    else g_jx = g_jy = 0;
    p.appliedX = g_jx, p.appliedY = g_jy, p.appliedFrame = g_frame;
    if (g_jx || g_jy) {
        const float p2 = P[2], p6 = P[6];
        memcpy(g_projSave, P, sizeof g_projSave);
        core::jitterProjectionMatrix(P, g_jx * g_jitterScale, g_jy * g_jitterScale, g_jitW, g_jitH);
        memcpy(g_projJittered, P, sizeof g_projJittered);
        g_projSaved = true;
        static LogRate jrate;
        if (logRate(jrate)) VRLOG("upint: flat jitter %.3f %.3f P2 %.6f->%.6f P6 %.6f->%.6f", g_jx, g_jy, p2, P[2], p6, P[6]);
    }
    g_size[0] = {uint32_t(rbw), uint32_t(rbh), uint32_t(bw), uint32_t(bh), g_frame};
    g_flatArmed = true;
}

// VR menu mode without phase B (no upscaler, or 100 %): the eyes ignore the user's MPix cap, which is meant for
// monitors. The HDR allocation of any VR view gets its input (it holds any smaller canvas). RenderView of an eye target
// gets the menu percentage: below 100 the engine renders its 3D canvas at that size and rescales it with the user's
// Rescaling Filter, as with a pixel ratio. Single pass renders both eyes in one RenderView with the device eye at 0
// (R2); it only runs at 100 %, so there the eye-size target keeps its size.
void vrStockLimit(void* ret, int w, int h, int* ow, int* oh) {
    const int eye = deviceEye();
    if (eye < 0) return;  // no device
    static LogRate rate;
    if (ret == g_hdrRet) {
        if ((*ow != w || *oh != h) && logRate(rate)) VRLOG("upint: VR hdr limiter: identity %dx%d (engine %dx%d)", w, h, *ow, *oh);
        *ow = w, *oh = h;
        return;
    }
    int ew, eh;
    vrdev::eyeTargetSize(ew, eh);
    if (w != ew || h != eh) return;
    uint32_t rw = uint32_t(w), rh = uint32_t(h);
    if (eye == 1 || eye == 2) vrRenderSize(uint32_t(w), uint32_t(h), rw, rh);
    static LogRate first;
    if (first.n < 2 && logRate(first))
        VRLOG("upint: VR limiter: eye %d %dx%d engine %dx%d (MPix %.2f, pixel ratio %.3f) -> %ux%u", eye, w, h, *ow, *oh, g_maxMpix ? *g_maxMpix : -1.0f,
              g_pixelRatio ? *g_pixelRatio : -1.0f, rw, rh);
    if ((int(rw) != *ow || int(rh) != *oh) && logRate(rate))
        VRLOG("upint: VR limiter: eye %d %dx%d -> %ux%u (%d%%, %s; engine %dx%d)", eye, w, h, rw, rh, eye == 1 || eye == 2 ? g_vrPct.load() : 100,
              up::name(), *ow, *oh);
    *ow = int(rw), *oh = int(rh);
}

float __cdecl hookedLimiter(int w, int h, int* ow, int* oh) {
    const float r = realLimiter(w, h, ow, oh);  // RenderView scales the shadow maps with it below 1: kept as is
    void* ret = __builtin_return_address(0);    // MinHook enters by a jmp: the engine caller's return address
    if (ret != g_rvRet && ret != g_hdrRet) return r;
    if (ret == g_rvRet && t_flatMain) {  // desktop: the flat main view's RenderView (no VR device)
        flatLimiter(w, h, ow, oh);
        return r;
    }
    if (ret == g_hdrRet && !phaseB() && deviceEye() == -1 && flatEnabled()) {
        // Flat phase A: the main view renders its full box, so its HDR canvas is not limited either (R27 s4.3).
        static LogRate rate;
        if (logRate(rate)) VRLOG("upint: hdr limiter: identity %dx%d (engine %dx%d, flat)", w, h, *ow, *oh);
        *ow = w, *oh = h;
        return r;
    }
    if (!phaseB()) {
        if (g_vrMenu) vrStockLimit(ret, w, h, ow, oh);
        return r;
    }
    if (ret == g_hdrRet) {
        // The HDR canvas never shrinks below its input (the eye size of slot 0x068), for every view: a smaller limit
        // would re-create it every frame (R25 s2.2).
        static LogRate rate;
        if (logRate(rate)) VRLOG("upint: hdr limiter: identity %dx%d (engine %dx%d)", w, h, *ow, *oh);
        *ow = w, *oh = h;
        return r;
    }
    const int eye = deviceEye();
    if (eye != 1 && eye != 2) return r;
    int ew, eh;
    vrdev::eyeTargetSize(ew, eh);
    static LogRate rate;
    if (w != ew || h != eh) {
        // Not the eye render target (another target drawn during the eye pass): left to the engine.
        if (logRate(rate)) VRLOG("upint: limiter: eye %d %dx%d is not the eye size %dx%d, unchanged", eye, w, h, ew, eh);
        return r;
    }
    uint32_t rw, rh;
    vrRenderSize(uint32_t(w), uint32_t(h), rw, rh);  // replaces the user's MPix limit and pixel ratio for the eye
    if (logRate(rate))
        VRLOG("upint: limiter: eye %d %dx%d -> %ux%u (ret %p, engine %dx%d, projection %ux%u)", eye, w, h, rw, rh, ret, *ow, *oh,
              g_proj[eye].rw, g_proj[eye].rh);
    *ow = int(rw), *oh = int(rh);
    g_size[eye] = {rw, rh, uint32_t(w), uint32_t(h), g_frame};
    return r;
}

void* hookedHdr(uint32_t a) {
    g_hdrThread = GetCurrentThreadId();
    // Flat phase B (R27 s4.2): RenderView made the render-size 3D canvas current, and hdr_targets would size the HDR
    // canvas from it. It sees the main drawport (RenderView's saved dp) instead, so the canvas stays at the backbuffer
    // size and post-processing can run at the output size.
    void** cur = nullptr;
    void* saved = nullptr;
    if (t_flatMain && g_flatArmed && phaseB()) {
        uint8_t* gfx = gfxObject();
        void** c = gfx ? reinterpret_cast<void**>(gfx + kGfxCurrentDp) : nullptr;
        void* mainDp = *g_eyeDpGlobal;
        if (c && *c == g_canvas3dDp && mainDp) {
            cur = c, saved = *c;
            *c = mainDp;
            static LogRate rate;
            if (logRate(rate)) VRLOG("upint: flat hdr: current dp 3D canvas %p -> saved dp %p", saved, mainDp);
        }
    }
    void* r = realHdr(a);
    if (cur) *cur = saved;
    g_hdrThread = 0;
    return r;
}

// Execute-view rescales the 3D canvas into the eye drawport after the view (rescale_pass or gfx_copy). Phase B's
// post-processing already wrote the eye drawport at full size, so that one rescale is skipped (R25 s3.2).
bool skipRescale(const void* src, const void* dst, const char* what) {
    if (!t_ppWroteEye || src != g_canvas3dDp || dst != *g_eyeDpGlobal) return false;
    t_ppWroteEye = false;  // one rescale per post-processed eye view
    const LONG n = InterlockedIncrement(&g_skips);
    static LogRate rate;
    const int eye = deviceEye();
    if (logRate(rate)) {
        if (eye == -1) VRLOG("upint: rescale skipped (flat, %s) (%ld skips, %ld phase B dispatches)", what, n, g_eyePp);
        else VRLOG("upint: rescale skipped (%s) eye %d (%ld skips, %ld phase B eye dispatches)", what, eye, n, g_eyePp);
    }
    return true;
}
uintptr_t __cdecl hookedRescale(void* src, void* dst, uintptr_t mode) { return skipRescale(src, dst, "pass") ? 0 : realRescale(src, dst, mode); }
uintptr_t __cdecl hookedCopy(void* gfx, void* src, uintptr_t a, void* dst, uintptr_t b, uintptr_t filtered) {
    return skipRescale(src, dst, "copy") ? 0 : realCopy(gfx, src, a, dst, b, filtered);
}

// Phase B post-processing of a view (R25 s5 step 6; flat: R27 s4.2): the HDR canvas box becomes the output size, the
// upscaler fills it at the PP's first canvas barrier (onImageBarrier -> evaluateEye), and the PP writes the saved
// drawport (the eye, or the flat main dp) instead of the 3D canvas. Any mismatch runs the stock PP: the engine then
// rescales the render-size image spatially.
void ppPhaseB(void* gfx, void* dst, uint32_t flags, void* p4, int slot, VkCommandBuffer cb) {
    const EyeSize s = g_size[slot];
    void* eyeDp = *g_eyeDpGlobal;
    int rw, rh, ow = 0, oh = 0, bw = 0, bh = 0;
    boxSize(dst, rw, rh);  // view+0x288, the copy of the 3D canvas drawport
    if (eyeDp) boxSize(eyeDp, ow, oh);
    const char* why = nullptr;
    if (!armed(slot)) why = "eye not armed";
    else if (uint32_t(rw) != s.rw || uint32_t(rh) != s.rh) why = "render size";
    else if (!eyeDp || uint32_t(ow) != s.ow || uint32_t(oh) != s.oh) why = "eye drawport";
    else if (g_color.w < uint32_t(ow) || g_color.h < uint32_t(oh)) why = "canvas smaller than the eye";
    else if (g_color.format != VK_FORMAT_R16G16B16A16_SFLOAT) why = "canvas format";
    else if (!ensureResources() || !ensureOutput(uint32_t(ow), uint32_t(oh))) why = "resources";
    else {
        uint8_t* hdp = hdrSubrect(1, ow, oh);
        if (hdp) boxSize(hdp, bw, bh);
        if (hdp != g_hdrCanvasDp || bw != ow || bh != oh) {
            hdrSubrect(1, rw, rh);
            why = "canvas box";
        }
    }
    static LogRate rate;
    if (logRate(rate))
        VRLOG("upint: pp: %s dst %dx%d eye %dx%d canvas %ux%u box %dx%d -> %s%s%s", slotName(slot), rw, rh, ow, oh, g_color.w, g_color.h, bw, bh,
              why ? "stock(" : "phaseB", why ? why : "", why ? ")" : "");
    ++t_ppDepth;
    t_ppFlags = flags;
    t_ppCb = cb;
    t_pendingSlot = 0;
    if (why) {
        realPp(gfx, dst, flags, p4);
        --t_ppDepth;
        return;
    }
    t_phaseB = {slot, uint32_t(rw), uint32_t(rh), uint32_t(ow), uint32_t(oh), false};
    t_pendingSlot = slot + 1;
    realPp(gfx, eyeDp, flags, p4);
    const bool filled = t_phaseB.filled;
    if (t_pendingSlot) {
        static volatile LONG logged;
        if (InterlockedIncrement(&logged) <= 3) VRLOG("upint: %s: no canvas barrier seen in post-processing", slotName(t_pendingSlot - 1));
    }
    t_phaseB = {};
    t_pendingSlot = 0;
    hdrSubrect(1, rw, rh);  // the engine's box again for the rest of the view
    --t_ppDepth;
    if (filled) {
        InterlockedIncrement(&g_eyePp);
        t_ppWroteEye = true;
        return;
    }
    // The canvas never got its output-size image, so the eye drawport holds a partial one. Post-process again the stock
    // way into the 3D canvas; the engine's rescale then overwrites the eye drawport.
    static LogRate redo;
    if (logRate(redo)) VRLOG("upint: %s phase B canvas not filled - stock post-processing again", slotName(slot));
    realPp(gfx, dst, flags, p4);
}

// The flat main view's post-processing (R27 s3.2): the PP command's call (ppcmd_pp_call + 5) with dst = view + 0x288
// of this frame's recorded main view. The video/image PP (fmv_pp_call) and other callers are never upscaled. Phase A
// evaluates the HDR box (t_flatBoxW/H), which must start at (0,0) and fit the canvas.
bool flatPp(void* ret, void* dst, uint32_t flags, bool want) {
    if (ret != g_ppCmdRet) {
        static LogRate rate;
        if (logRate(rate)) VRLOG("upint: pp: fmv/other ret %p skipped (%s)", ret, ret == g_fmvRet ? "fmv" : "other");
        return false;
    }
    int bw = 0, bh = 0;
    if (g_hdrCanvasDp) boxSize(g_hdrCanvasDp, bw, bh);
    const bool origin = g_hdrCanvasDp && boxAtOrigin(g_hdrCanvasDp);
    const char* why = nullptr;
    if (dst != g_flat.dst) why = "dst";
    else if (!g_flatArmed || g_flat.frame != g_frame) why = "view not armed";
    else if (!want) why = (flags & 1) ? "flags" : LONG(flags) != g_ppFlags ? "not the canvas dispatch" : "upscaler idle";
    else if (!phaseB() && (!origin || bw <= 0 || bh <= 0 || uint32_t(bw) > g_color.w || uint32_t(bh) > g_color.h)) why = "hdr box";
    static LogRate rate;
    if (logRate(rate))
        VRLOG("upint: pp: flat ret %p dst %p (expect %p) flags 0x%x hdr box %dx%d canvas %ux%u -> %s%s%s", ret, dst, g_flat.dst, flags, bw, bh,
              g_color.w, g_color.h, why ? "stock(" : "evaluate", why ? why : "", why ? ")" : "");
    t_flatBoxW = bw, t_flatBoxH = bh;
    return !why;
}

void hookedPp(void* gfx, void* dst, uint32_t flags, void* p4) {
    if (g_extraBias) *g_extraBias = 0.0f;  // post-processing and what follows in the view are unbiased
    void* ret = __builtin_return_address(0);  // MinHook enters by a jmp: the engine caller's return address
    const int eye = deviceEye();
    const VkCommandBuffer cb = engineCommandBuffer(gfx);
    // The scene pass is usually still open here (the engine ends passes lazily): the work is recorded at the PP's
    // own first canvas barrier instead (onImageBarrier), where no pass can be open.
    const bool want = g_evaluate && g_evaluateOn && up::active() != up::Kind::Off && (flags & 1) == 0 && LONG(flags) == g_ppFlags &&
                      !t_ppDepth && cb;
    int slot = -1;  // the view this dispatch post-processes: VR eye 1/2, or the flat main view 0
    if (eye == 1 || eye == 2) {
        if (want && player_vr::localVrd()) slot = eye;
    } else if (eye == -1 && !t_ppDepth && flatEnabled() && flatPp(ret, dst, flags, want))
        slot = 0;
    if (slot >= 0 && phaseB()) {
        ppPhaseB(gfx, dst, flags, p4, slot, cb);
        return;
    }
    ++t_ppDepth;
    t_ppFlags = flags;
    t_ppCb = cb;
    t_pendingSlot = slot >= 0 ? slot + 1 : 0;
    realPp(gfx, dst, flags, p4);
    if (t_pendingSlot) {
        static volatile LONG logged;
        if (InterlockedIncrement(&logged) <= 3) VRLOG("upint: %s: no canvas barrier seen in post-processing", slotName(t_pendingSlot - 1));
    }
    t_pendingSlot = 0;
    --t_ppDepth;
}

int32_t rel32(const uint8_t* p) {
    int32_t v;
    memcpy(&v, p, 4);
    return v;
}
// `site` is an E8 rel32 call of `target`.
bool callsTo(const uint8_t* site, const void* target) { return site[0] == 0xE8 && site + 5 + rel32(site + 1) == target; }

// R25 s5 step 1: symbols, call sites, and the two drawport globals decoded from verified instructions. Any failure
// leaves phase B off (the eyes render at output size, phase A).
bool installPhaseB() {
    const auto& b = se4::build();
    auto* lim = b.at<uint8_t>(se4::gen::mpix_limiter);
    auto* rvCall = b.at<uint8_t>(se4::gen::rv_limiter_call);
    auto* hdrCall = b.at<uint8_t>(se4::gen::hdr_limiter_call);
    auto* sub = b.at<uint8_t>(se4::gen::hdr_subrect);
    auto* rescale = b.at<uint8_t>(se4::gen::rescale_pass);
    auto* copy = b.at<uint8_t>(se4::gen::gfx_copy);
    auto* copyCall = b.at<uint8_t>(se4::gen::exec_copy_call);
    auto* rescaleSite = b.at<uint8_t>(se4::gen::exec_rescale_site);  // lea rcx, [3D canvas dp]; call rescale_pass
    auto* eyeLoad = b.at<uint8_t>(se4::gen::exec_eye_dp_load);       // mov rdx, [saved eye dp]
    auto* canvas = b.at<uint8_t>(se4::gen::hdr_canvas);
    const char* why = nullptr;
    if (!lim || !rvCall || !hdrCall || !sub || !rescale || !copy || !copyCall || !rescaleSite || !eyeLoad || !canvas ||
        !b.at<void>(se4::gen::execute_view))
        why = "symbols missing in this build";
    else if (!callsTo(rvCall, lim) || !callsTo(hdrCall, lim)) why = "limiter call sites do not match";
    else if (!callsTo(copyCall, copy)) why = "execute-view copy call does not match";
    else if (rescaleSite[0] != 0x48 || rescaleSite[1] != 0x8D || rescaleSite[2] != 0x0D || !callsTo(rescaleSite + 7, rescale))
        why = "execute-view rescale site does not match";
    else if (eyeLoad[0] != 0x48 || eyeLoad[1] != 0x8B || eyeLoad[2] != 0x15) why = "execute-view eye drawport load does not match";
    else if (se4::options::get("single_pass", 0)) why = "single-pass stereo (a view's eye is not known at RenderView)";
    if (why) return VRLOG("upint: phase B off: %s", why), false;
    g_canvas3dDp = rescaleSite + 7 + rel32(rescaleSite + 3);
    g_eyeDpGlobal = reinterpret_cast<void**>(eyeLoad + 7 + rel32(eyeLoad + 3));
    g_rvRet = rvCall + 5, g_hdrRet = hdrCall + 5;
    g_hdrCanvasDp = canvas;
    hdrSubrect = reinterpret_cast<HdrSubrectFn>(sub);
    auto hook = [](void* at, void* fn, void** real) { return MH_CreateHook(at, fn, real) == MH_OK && MH_EnableHook(at) == MH_OK; };
    if (!hook(lim, reinterpret_cast<void*>(hookedLimiter), reinterpret_cast<void**>(&realLimiter)) ||
        !hook(rescale, reinterpret_cast<void*>(hookedRescale), reinterpret_cast<void**>(&realRescale)) ||
        !hook(copy, reinterpret_cast<void*>(hookedCopy), reinterpret_cast<void**>(&realCopy)))
        return VRLOG("upint: phase B off: hooks FAILED"), false;
    g_phaseBReady = true;
    VRLOG("upint: phase B ready (3D canvas dp %p, eye dp global %p, limiter returns %p / %p)", static_cast<void*>(g_canvas3dDp),
          static_cast<void*>(g_eyeDpGlobal), g_rvRet, g_hdrRet);
    return true;
}

// R27 s5 step 1 (desktop mode only): symbols, call sites, gfx offsets and the globals decoded from verified
// instructions. Any failure leaves flat upscaling off and the game renders stock.
bool installFlat() {
    const auto& b = se4::build();
    auto* rv = b.at<uint8_t>(se4::gen::render_view);
    auto* views = b.at<uint8_t>(se4::gen::player_views);
    auto* endView = b.at<uint8_t>(se4::gen::end_view);
    auto* mainStore = b.at<uint8_t>(se4::gen::render_maindp_store);  // movups [main dp], xmm3
    auto* bbGet = b.at<uint8_t>(se4::gen::gfx_backbuffer_get);       // mov rax, [backbuffer texture]; ret
    auto* viewStore = b.at<uint8_t>(se4::gen::rv_viewcmd_store);     // mov [view command], rcx
    auto* viewCopy = b.at<uint8_t>(se4::gen::rv_view_copy);          // movsd xmm0, [rax+0xc70]
    auto* projCopy = b.at<uint8_t>(se4::gen::rv_proj_copy);          // movups xmm0, [rax+0xc2c]
    auto* ppCall = b.at<uint8_t>(se4::gen::ppcmd_pp_call);
    auto* pvCall = b.at<uint8_t>(se4::gen::pv_rv_call);
    auto* camCall = b.at<uint8_t>(se4::gen::cam_rv_call);
    auto* fmvCall = b.at<uint8_t>(se4::gen::fmv_pp_call);
    auto* lim = b.at<uint8_t>(se4::gen::mpix_limiter);
    auto* rvCall = b.at<uint8_t>(se4::gen::rv_limiter_call);
    auto* hdrCall = b.at<uint8_t>(se4::gen::hdr_limiter_call);
    auto* pp = b.at<uint8_t>(se4::gen::pp_dispatch);
    auto* eyeLoad = b.at<uint8_t>(se4::gen::exec_eye_dp_load);  // +0x1c: mov [gfx], rdi (R25 s7 "global: gfx")
    auto* canvas = b.at<uint8_t>(se4::gen::hdr_canvas);
    static const uint8_t kProjCopy[7] = {0x0F, 0x10, 0x80, 0x2C, 0x0C, 0x00, 0x00};
    static const uint8_t kViewCopy[8] = {0xF2, 0x0F, 0x10, 0x80, 0x70, 0x0C, 0x00, 0x00};
    const char* why = nullptr;
    if (!rv || !views || !endView || !mainStore || !bbGet || !viewStore || !viewCopy || !projCopy || !ppCall || !pvCall || !camCall || !lim ||
        !rvCall || !hdrCall || !pp || !eyeLoad || !canvas)
        why = "symbols missing in this build";
    else if (mainStore[0] != 0x0F || mainStore[1] != 0x11 || mainStore[2] != 0x1D) why = "main drawport store does not match";
    else if (bbGet[0] != 0x48 || bbGet[1] != 0x8B || bbGet[2] != 0x05 || bbGet[7] != 0xC3) why = "backbuffer getter does not match";
    else if (viewStore[0] != 0x48 || viewStore[1] != 0x89 || viewStore[2] != 0x0D) why = "view command store does not match";
    else if (memcmp(projCopy, kProjCopy, sizeof kProjCopy)) why = "RenderView projection copy (gfx+0xc2c) does not match";
    else if (memcmp(viewCopy, kViewCopy, sizeof kViewCopy)) why = "RenderView world-to-view copy (gfx+0xc70) does not match";
    else if (!callsTo(ppCall, pp)) why = "PP command call does not match";
    else if (!callsTo(pvCall, rv) || !callsTo(camCall, rv)) why = "render_view call sites do not match";
    else if (!callsTo(pvCall + 0x40, endView)) why = "player view end_view call does not match";
    else if (!callsTo(rvCall, lim) || !callsTo(hdrCall, lim)) why = "limiter call sites do not match";
    else if (eyeLoad[0x1c] != 0x48 || eyeLoad[0x1d] != 0x89 || eyeLoad[0x1e] != 0x3D) why = "gfx global store does not match";
    if (why) return VRLOG("upint: flat off: %s", why), false;
    g_mainDp = mainStore + 7 + rel32(mainStore + 3);
    g_backbufferTex = reinterpret_cast<void**>(bbGet + 7 + rel32(bbGet + 3));
    g_viewCmdGlobal = reinterpret_cast<void**>(viewStore + 7 + rel32(viewStore + 3));
    g_gfxGlobal = reinterpret_cast<void**>(eyeLoad + 0x1c + 7 + rel32(eyeLoad + 0x1f));
    g_pvRet = pvCall + 5, g_camRet = camCall + 5, g_ppCmdRet = ppCall + 5;
    g_fmvRet = fmvCall && callsTo(fmvCall, pp) ? fmvCall + 5 : nullptr;  // logging only
    g_rvRet = rvCall + 5, g_hdrRet = hdrCall + 5;
    g_hdrCanvasDp = canvas;
    g_aaSamples = b.at<int32_t>(se4::gen::gfx_iAntiAliasingSamples);
    g_maxRt = b.at<int32_t>(se4::gen::gfx_pixMaxRenderTargetSize);
    g_forceCas = b.at<int32_t>(se4::gen::ren_bForceCAS);
    auto hook = [](void* at, void* fn, void** real) { return MH_CreateHook(at, fn, real) == MH_OK && MH_EnableHook(at) == MH_OK; };
    // The limiter is already hooked when phase B is installed.
    const MH_STATUS limState = realLimiter ? MH_EnableHook(lim) : MH_UNKNOWN;
    bool ok = realLimiter ? (limState == MH_OK || limState == MH_ERROR_ENABLED)
                          : hook(lim, reinterpret_cast<void*>(hookedLimiter), reinterpret_cast<void**>(&realLimiter));
    ok = ok && hook(views, reinterpret_cast<void*>(hookedPlayerViews), reinterpret_cast<void**>(&realPlayerViews)) &&
         hook(endView, reinterpret_cast<void*>(hookedEndView), reinterpret_cast<void**>(&realEndView)) &&
         hook(rv, reinterpret_cast<void*>(hookedRenderView), reinterpret_cast<void**>(&realRenderView));
    if (!ok) return VRLOG("upint: flat off: hooks FAILED"), false;
    if (!g_aaSamples) VRLOG("upint: flat: gfx_iAntiAliasingSamples is not in this build - MSAA is not checked");
    g_flatReady = true;
    VRLOG("upint: flat ready (main dp %p, backbuffer texture global %p, view command global %p, gfx global %p, MSAA %d; render_view returns "
          "player %p camera %p, PP command return %p, video PP return %p)",
          static_cast<void*>(g_mainDp), static_cast<void*>(g_backbufferTex), static_cast<void*>(g_viewCmdGlobal), static_cast<void*>(g_gfxGlobal),
          g_aaSamples ? *g_aaSamples : -1, g_pvRet, g_camRet, g_ppCmdRet, g_fmvRet);
    return true;
}
}  // namespace

bool phaseB() {
    if (!g_phaseBReady) return false;
    auto* singlePass = se4::build().at<int32_t>(se4::gen::vr_iUsingSinglePassStereo);
    const char* why = nullptr;
    if (!g_evaluate) why = "upscaler disabled";
    else if (!g_evaluateOn) why = "upscale_evaluate=0";
    else if (up::active() == up::Kind::Off) why = up::menuMode() ? "upscaler off (Rescaling Filter menu)" : "no upscaler backend";
    else if (g_vrMenu ? g_vrPct.load() >= 100 : g_menuSize ? !g_flatScaled : up::ratio() <= 1.0f)
        why = g_vrMenu ? "VR render resolution 100%" : g_menuSize ? "engine render size = output" : "ratio 1";
    else if (singlePass && *singlePass > 0) why = "single-pass stereo";
    // Logged once per change of state.
    static PVOID volatile logged;
    PVOID state = const_cast<char*>(why ? why : "on");
    if (InterlockedExchangePointer(&logged, state) != state) {
        if (why) VRLOG("upint: phase B off: %s", why);
        else if (g_vrMenu) VRLOG("upint: phase B on: %s at %d%% of the eye size", up::name(), g_vrPct.load());
        else if (g_menuSize) VRLOG("upint: phase B on: %s at the engine's render size", up::name());
        else VRLOG("upint: phase B on: %s at ratio %.2f", up::name(), up::ratio());
    }
    return !why;
}

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
    F(blit, vkCmdBlitImage);
    F(clear, vkCmdClearColorImage);
    F(createBuffer, vkCreateBuffer);
    F(bufReq, vkGetBufferMemoryRequirements);
    F(bindBuf, vkBindBufferMemory);
    F(map, vkMapMemory);
    F(copyToBuf, vkCmdCopyImageToBuffer);
#undef F
}

void patchImage(VkImageCreateInfo& ci) {
    if (!g_evaluate || !g_hdrThread || g_hdrThread != GetCurrentThreadId()) return;
    if (isDepth(ci.format)) ci.usage |= VK_IMAGE_USAGE_SAMPLED_BIT;
    // Phase B's fallback blits from the canvas.
    // Desktop menu mode can enter phase B at any time without the canvas being re-created (it stays at the backbuffer size).
    else if (ci.format == VK_FORMAT_R16G16B16A16_SFLOAT && ci.samples == VK_SAMPLE_COUNT_1_BIT && (phaseB() || (up::menuMode() && g_phaseBReady)))
        ci.usage |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
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

// ---- lab pass trace ----
namespace {
struct RpInfo {
    int depth = -1;
    VkAttachmentLoadOp load = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    VkAttachmentStoreOp store = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    int colors = 0, hdr = 0;
};
std::mutex g_traceLock;
std::unordered_map<VkRenderPass, RpInfo> g_rps;
std::unordered_map<VkImageView, VkImage> g_views;
std::unordered_map<VkFramebuffer, std::vector<VkImageView>> g_fbs;
template <class A> void recordRp(VkRenderPass rp, const A* a, uint32_t n) {
    RpInfo r;
    for (uint32_t i = 0; i < n; ++i) {
        if (isDepth(a[i].format)) r.depth = int(i), r.load = a[i].loadOp, r.store = a[i].storeOp;
        else ++r.colors, r.hdr += a[i].format == VK_FORMAT_R16G16B16A16_SFLOAT;
    }
    std::lock_guard<std::mutex> l(g_traceLock);
    g_rps[rp] = r;
}
}  // namespace
void onViewport(float minDepth, float maxDepth) {
    if (maxDepth == 1.0f && minDepth > 0.0f && minDepth < 0.5f && g_depthSplit.load(std::memory_order_relaxed) != minDepth) {
        g_depthSplit = minDepth;
        VRLOG("upint: scene depth partition at %.3f (world in [%.3f, 1]): motion vectors decode it", minDepth, minDepth);
    }
}
bool passTraceOn() {
    if (g_passTrace < 0) g_passTrace = std::max(0, se4::options::get("upscale_pass_trace", 0));
    return g_passTrace > 0;
}
void traceRenderPass(VkRenderPass rp, const VkAttachmentDescription* a, uint32_t n) {
    if (passTraceOn()) recordRp(rp, a, n);
}
void traceRenderPass2(VkRenderPass rp, const VkAttachmentDescription2* a, uint32_t n) {
    if (passTraceOn()) recordRp(rp, a, n);
}
void traceView(VkImageView view, VkImage image) {
    std::lock_guard<std::mutex> l(g_traceLock);
    g_views[view] = image;
}
void traceFramebuffer(VkFramebuffer fb, VkRenderPass, const VkImageView* views, uint32_t n) {
    std::lock_guard<std::mutex> l(g_traceLock);
    g_fbs[fb].assign(views, views + n);
}
void traceBeginPass(VkCommandBuffer cb, const VkRenderPassBeginInfo* bi) {
    if (!passTraceOn() || !bi || !g_depth.image || g_traceFrames > g_passTrace) return;
    std::lock_guard<std::mutex> l(g_traceLock);
    auto fb = g_fbs.find(bi->framebuffer);
    if (fb == g_fbs.end()) return;
    bool depth = false, color = false;
    for (VkImageView v : fb->second) {
        auto it = g_views.find(v);
        if (it == g_views.end()) continue;
        depth |= it->second == g_depth.image;
        color |= it->second == g_color.image;
    }
    if (!depth) return;
    const RpInfo r = g_rps.count(bi->renderPass) ? g_rps[bi->renderPass] : RpInfo{};
    VRLOG("passtrace: frame %ld #%ld cb %p pass %p depth load %d store %d colors %d (hdr %d, canvas %d) area %dx%d+%d+%d", g_traceFrames,
          InterlockedIncrement(&g_traceSeq), static_cast<void*>(cb), reinterpret_cast<void*>(bi->renderPass), int(r.load), int(r.store), r.colors,
          r.hdr, int(color), int(bi->renderArea.extent.width), int(bi->renderArea.extent.height), bi->renderArea.offset.x, bi->renderArea.offset.y);
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
    if (t_pendingSlot) {
        const int slot = t_pendingSlot - 1;
        t_pendingSlot = 0;
        bool open;
        {
            std::lock_guard<std::mutex> l(g_passLock);
            open = g_openPass.count(cb) != 0;
        }
        if (from == VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL && !open) evaluateEye(cb, slot);
        else {
            static volatile LONG logged;
            if (InterlockedIncrement(&logged) <= 3)
                VRLOG("upint: %s not evaluated (canvas layout %d, pass open %d)", slotName(slot), int(from), int(open));
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
        g_jitW = g_color.w, g_jitH = g_color.h, g_jitOut = g_color.w;
        if (phaseB()) {
            // The size the limiter will return for this eye's RenderView, which runs after the projection (R25 s4).
            int ew, eh;
            vrdev::eyeTargetSize(ew, eh);
            vrRenderSize(uint32_t(ew), uint32_t(eh), g_jitW, g_jitH);
            g_jitOut = uint32_t(ew);
        }
        if (g_evaluate && g_jitterOn && up::active() != up::Kind::Off && g_jitW) up::jitter(g_frame, g_jitW, g_jitOut, g_jx, g_jy);
        else g_jx = g_jy = 0;
    }
    ProjRec& p = g_proj[eye];
    p.appliedX = g_jx, p.appliedY = g_jy, p.appliedFrame = g_frame;
    p.rw = g_jitW, p.rh = g_jitH;
    p.tan = core::tangents(fov);
    p.n = n, p.f = f;
    p.valid = true;
    if (!g_jx && !g_jy) return fov;
    return core::angles(core::jittered(p.tan, g_jx * g_jitterScale, g_jy * g_jitterScale, g_jitW, g_jitH));
}

bool flatWanted() { return !se4::g_vrEnabled && up::wanted(); }
bool vrMenu() { return g_vrMenu; }
bool vrSizeReady() { return g_vrMenu && g_phaseBReady; }
void setVrPercent(int pct) {
    pct = std::clamp(pct, 1, 100);
    if (g_vrPct.exchange(pct) != pct) g_vrDirty = true;
}
int vrPercent() { return g_vrPct.load(); }
void onVrFrameBegin() {
    if (g_vrMenu) applyVrRequest();
    foliage::update(g_evaluate && up::active() != up::Kind::Off);
}
bool flatReady() { return g_flatReady; }
void requestKind(up::Kind k) { g_request = int(k); }

bool install() {
    g_evaluate = up::wanted();
    if (!g_evaluate) return false;
    g_jitterOn = se4::options::get("upscale_jitter", 1) != 0;
    g_evaluateOn = se4::options::get("upscale_evaluate", 1) != 0;
    g_lodBias = se4::options::get("upscale_lod_bias", -100) / 100.0f;
    g_jitterScale = se4::options::get("upscale_jitter_scale", 100) / 100.0f;
    g_motionOn = se4::options::get("upscale_mv", 1) != 0;
    g_debugView = std::clamp(se4::options::get("upscale_debug_view", 0), 0, 2);
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
    // Phase B (render at out/ratio) for the quality modes 1..3 unless upscale_phase_b=0. Desktop menu mode always
    // installs it: the user can lower "Max 3D rendering Mpix" at runtime, which makes the engine's render size smaller.
    const int quality = std::clamp(se4::options::get("upscale_quality", 0), 0, 3);
    const bool menu = up::menuMode();
    g_vrMenu = menu && se4::g_vrEnabled;
    g_menuSize = menu && !se4::g_vrEnabled && !se4::options::has("upscale_quality");
    g_singlePassForced = se4::options::has("single_pass") ? (se4::options::get("single_pass", 0) != 0) : -1;
    if (g_vrMenu) VRLOG("upint: VR menu mode: eye render size %d%% (Options > Graphics, the VR render resolution row)", g_vrPct.load());
    else if (menu) VRLOG("upint: desktop menu mode: render size from %s", g_menuSize ? "the engine's Max 3D rendering Mpix" : "upscale_quality (options.txt)");
    g_maxMpix = b.at<float>(se4::gen::ren_fMax3DRenderingMPix);
    g_pixelRatio = b.at<float>(se4::gen::ren_fRenderingPixelRatio);
    if (!se4::options::get("upscale_phase_b", 1)) VRLOG("upint: phase B off: upscale_phase_b=0");
    else if (!quality && !menu) VRLOG("upint: phase B off: upscale_quality 0 (native AA at the eye size)");
    else installPhaseB();
    // Desktop play: the flat main view as slot 0 (docs/re/R27_flat_upscaler.md). Never in VR mode.
    if (flatWanted()) installFlat();
    if (g_evaluate) installLodBias();
    return true;
}
}  // namespace upint
