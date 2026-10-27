// Foveated rendering via VK_KHR_fragment_shading_rate attachments. Scene render passes (a depth attachment
// and at least one colour attachment) get one extra attachment: an R8_UINT shading-rate image. Framebuffers
// of the eye render size use the foveation pattern; every other framebuffer of such a pass uses a neutral
// (full-rate) image. Pipelines of those passes combine the engine's own per-draw rate with the attachment
// (coarser wins), so the engine's VRS menu options keep working.
#include "vk/foveation.hpp"
#include "se4/log.hpp"
#include "se4/options.hpp"
#include "se4/vr_device.hpp"
#include "vk/queue_lock.hpp"
#include "xr/input.hpp"
#include "xr/runtime.hpp"
#include <windows.h>
#include <algorithm>
#include <atomic>
#include <cctype>
#include <cmath>
#include <cstring>
#include <mutex>
#include <new>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace fov {
namespace {
std::atomic<Mode> g_mode{Mode::Off};
int g_startChoice = -1;  // the VR menu's saved choice (gfxmenu::prepare), used when options.txt has no foveation
bool g_trace;  // lab: lab_fov_trace=1
bool g_labGaze;   // lab: lab_fov_gaze=1, a simulated gaze
bool g_allow4x4;  // option foveation_coarse=4
bool g_gaze;  // the runtime offers XR_EXT_eye_gaze_interaction
bool g_active;  // device supports it and the device was patched
VkDevice g_dev;
VkPhysicalDevice g_pd;
VkQueue g_queue;
uint32_t g_family;
PFN_vkGetDeviceProcAddr g_gdpa;
uint32_t g_texel = 16;
bool g_have4x4;
VkFragmentShadingRateCombinerOpKHR g_combine = VK_FRAGMENT_SHADING_RATE_COMBINER_OP_REPLACE_KHR;
float g_inner = 0.20f, g_outer = 0.40f;            // eye-tracked: a tight fovea that follows the gaze
float g_fixedInner = 0.30f, g_fixedOuter = 0.50f;  // fixed: wider, the eyes look around within it
float g_rIn = 0.18f, g_rOut = 0.35f;               // the radii fillPattern uses (set per upload)
uint32_t g_eyeW, g_eyeH;

PFN_vkCreateRenderPass realCreateRenderPass;
PFN_vkCreateRenderPass2 realCreateRenderPass2;
PFN_vkCreateFramebuffer realCreateFramebuffer;
PFN_vkCreateGraphicsPipelines realCreateGraphicsPipelines;
PFN_vkCmdSetFragmentShadingRateKHR realCmdSetFsr;
PFN_vkCmdBindPipeline realCmdBindPipeline;
// Pipelines whose fragment shading rate is dynamic state: their combiner ops come from vkCmdSetFragmentShadingRateKHR,
// which the engine records only with its own VRS on (otherwise undefined: the attachment is ignored).
std::unordered_set<VkPipeline> g_dynFsr;
SRWLOCK g_dynLock = SRWLOCK_INIT;
thread_local VkExtent2D t_fsrSize{1, 1};  // the engine's last per-draw rate on this recording thread
thread_local VkFragmentShadingRateCombinerOpKHR t_fsrOp0 = VK_FRAGMENT_SHADING_RATE_COMBINER_OP_KEEP_KHR;
PFN_vkCmdBeginRenderPass realCmdBeginRenderPass;
PFN_vkCmdBeginRenderPass2 realCmdBeginRenderPass2;

struct PassInfo {
    uint32_t fsrIndex;          // attachment index of the shading-rate image
    uint32_t subpassMask;       // subpasses that use it
};
std::mutex g_lock;
std::unordered_map<VkRenderPass, PassInfo> g_passes;
std::unordered_set<VkFramebuffer> g_imageless;

struct RateImage {
    VkImage image = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    uint32_t w = 0, h = 0;
};
RateImage g_neutral, g_eye;
RateImage g_wide;  // single-pass stereo: one double-wide scene target, eye 1 in the left half, eye 2 in the right
// Upload staging (host visible): neutral pattern at offset 0, then a ring of eye patterns.
VkBuffer g_staging;
VkDeviceMemory g_stagingMem;
uint8_t* g_stagingPtr;
constexpr uint32_t kNeutralTexels = 512;  // covers framebuffers up to 512 * texel (8192 px at 16)
constexpr uint32_t kRing = 8;  // two eye uploads per frame, several frames in flight
size_t g_eyeBytes;
uint32_t g_ring;
// What the eye image holds now (an upload is recorded only when the next eye needs something else).
struct Content {
    Mode mode = Mode::Off;
    float cx = -1, cy = -1, sx = 0, sy = 0;
    bool operator==(const Content& o) const {
        return mode == o.mode && std::fabs(cx - o.cx) < 0.005f && std::fabs(cy - o.cy) < 0.005f && std::fabs(sx - o.sx) < 0.001f &&
               std::fabs(sy - o.sy) < 0.001f;
    }
} g_content;
Content g_want;      // the pattern the eye being recorded needs (beforeEye), uploaded before its first scene pass
bool g_haveWant;
std::unordered_set<VkFramebuffer> g_eyeFbs;  // framebuffers whose shading-rate attachment is the eye image
volatile LONG g_eyeUploads;  // beforeEye calls since the last present (0: no per-eye views, record() updates instead)
Content g_wideContent[2];     // what each half of g_wide holds

template <class F> F dfn(const char* n) { return reinterpret_cast<F>(g_gdpa(g_dev, n)); }

uint32_t memoryType(uint32_t bits, VkMemoryPropertyFlags want, PFN_vkGetPhysicalDeviceMemoryProperties gmp) {
    VkPhysicalDeviceMemoryProperties mp;
    gmp(g_pd, &mp);
    for (uint32_t i = 0; i < mp.memoryTypeCount; ++i)
        if ((bits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & want) == want) return i;
    return UINT32_MAX;
}
PFN_vkGetPhysicalDeviceMemoryProperties g_gmp;

bool createRateImage(RateImage& r, uint32_t w, uint32_t h) {
    VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ci.imageType = VK_IMAGE_TYPE_2D;
    ci.format = VK_FORMAT_R8_UINT;
    ci.extent = {w, h, 1};
    ci.mipLevels = 1;
    ci.arrayLayers = 1;
    ci.samples = VK_SAMPLE_COUNT_1_BIT;
    ci.tiling = VK_IMAGE_TILING_OPTIMAL;
    ci.usage = VK_IMAGE_USAGE_FRAGMENT_SHADING_RATE_ATTACHMENT_BIT_KHR | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (dfn<PFN_vkCreateImage>("vkCreateImage")(g_dev, &ci, nullptr, &r.image) != VK_SUCCESS) return false;
    VkMemoryRequirements mr;
    dfn<PFN_vkGetImageMemoryRequirements>("vkGetImageMemoryRequirements")(g_dev, r.image, &mr);
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.allocationSize = mr.size;
    ai.memoryTypeIndex = memoryType(mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, g_gmp);
    if (ai.memoryTypeIndex == UINT32_MAX || dfn<PFN_vkAllocateMemory>("vkAllocateMemory")(g_dev, &ai, nullptr, &r.memory) != VK_SUCCESS)
        return false;
    dfn<PFN_vkBindImageMemory>("vkBindImageMemory")(g_dev, r.image, r.memory, 0);
    VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vi.image = r.image;
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vi.format = VK_FORMAT_R8_UINT;
    vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    if (dfn<PFN_vkCreateImageView>("vkCreateImageView")(g_dev, &vi, nullptr, &r.view) != VK_SUCCESS) return false;
    r.w = w, r.h = h;
    return true;
}

uint8_t rateCode(int log2w, int log2h) { return uint8_t((log2w << 2) | log2h); }

// Pattern: full rate within `inner` of the centre, 2x2 within `outer`, 4x4 (or 2x2) beyond. Distances are in
// units of the image height so the regions are round on screen.
// The rendered image covers the top-left sx x sy of the target (upscaler phase B renders a sub-rect); (cx, cy) and the
// radii are relative to it. Mode Off fills full rate.
// Columns [x0, x1) of a w-texel-wide image hold one view; dst rows are `stride` texels apart.
// 2x2 is the coarsest rate unless foveation_coarse=4: alpha-tested foliage discards per coarse fragment, so 4x4 gives
// blocky, crawling leaf edges in the periphery (which is sensitive to flicker). Below the output size (sx < 1: upscaler
// phase B) a 4x4 block would also cover ~6 output pixels after upscaling: 2x2 there in any case.
void fillRegion(uint8_t* dst, uint32_t stride, uint32_t x0, uint32_t x1, uint32_t h, float cx, float cy, float sx, float sy, bool off, float rIn,
                float rOut) {
    const uint8_t full = rateCode(0, 0), half = rateCode(1, 1), quarter = g_have4x4 && g_allow4x4 && sx > 0.999f ? rateCode(2, 2) : half;
    const uint32_t w = x1 - x0;
    for (uint32_t y = 0; y < h; ++y) {
        if (off) {
            memset(dst + size_t(y) * stride + x0, full, w);
            continue;
        }
        const float aspect = float(w) * sx / (float(h) * sy);
        for (uint32_t x = 0; x < w; ++x) {
            float dx = ((x + 0.5f) / w / sx - cx) * aspect, dy = (y + 0.5f) / h / sy - cy;
            float d = std::sqrt(dx * dx + dy * dy);
            dst[size_t(y) * stride + x0 + x] = d < rIn ? full : d < rOut ? half : quarter;
        }
    }
}
void fillPattern(uint8_t* dst, uint32_t w, uint32_t h, float cx, float cy, float sx = 1.0f, float sy = 1.0f, bool off = false) {
    fillRegion(dst, w, 0, w, h, cx, cy, sx, sy, off, g_rIn, g_rOut);
}

void barrier(VkCommandBuffer cb, VkImage img, VkImageLayout from, VkImageLayout to, VkAccessFlags src, VkAccessFlags dst,
             VkPipelineStageFlags ss, VkPipelineStageFlags ds) {
    VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    b.srcAccessMask = src, b.dstAccessMask = dst;
    b.oldLayout = from, b.newLayout = to;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = img;
    b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    dfn<PFN_vkCmdPipelineBarrier>("vkCmdPipelineBarrier")(cb, ss, ds, 0, 0, nullptr, 0, nullptr, 1, &b);
}

void copyIn(VkCommandBuffer cb, RateImage& r, VkDeviceSize offset, bool first) {
    barrier(cb, r.image, first ? VK_IMAGE_LAYOUT_UNDEFINED : VK_IMAGE_LAYOUT_FRAGMENT_SHADING_RATE_ATTACHMENT_OPTIMAL_KHR,
            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, first ? 0 : VK_ACCESS_FRAGMENT_SHADING_RATE_ATTACHMENT_READ_BIT_KHR,
            VK_ACCESS_TRANSFER_WRITE_BIT,
            first ? VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT : VK_PIPELINE_STAGE_FRAGMENT_SHADING_RATE_ATTACHMENT_BIT_KHR,
            VK_PIPELINE_STAGE_TRANSFER_BIT);
    VkBufferImageCopy c{};
    c.bufferOffset = offset;
    c.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    c.imageExtent = {r.w, r.h, 1};
    dfn<PFN_vkCmdCopyBufferToImage>("vkCmdCopyBufferToImage")(cb, g_staging, r.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &c);
    barrier(cb, r.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_FRAGMENT_SHADING_RATE_ATTACHMENT_OPTIMAL_KHR,
            VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_FRAGMENT_SHADING_RATE_ATTACHMENT_READ_BIT_KHR, VK_PIPELINE_STAGE_TRANSFER_BIT,
            VK_PIPELINE_STAGE_FRAGMENT_SHADING_RATE_ATTACHMENT_BIT_KHR);
}

// One-time synchronous upload when an image is created (it must be valid before the first scene pass).
void uploadNow(RateImage& r, VkDeviceSize offset) {
    VkCommandPool pool;
    VkCommandPoolCreateInfo pc{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pc.queueFamilyIndex = g_family;
    if (dfn<PFN_vkCreateCommandPool>("vkCreateCommandPool")(g_dev, &pc, nullptr, &pool) != VK_SUCCESS) return;
    VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    ai.commandPool = pool;
    ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    ai.commandBufferCount = 1;
    VkCommandBuffer cb;
    dfn<PFN_vkAllocateCommandBuffers>("vkAllocateCommandBuffers")(g_dev, &ai, &cb);
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    dfn<PFN_vkBeginCommandBuffer>("vkBeginCommandBuffer")(cb, &bi);
    copyIn(cb, r, offset, true);
    dfn<PFN_vkEndCommandBuffer>("vkEndCommandBuffer")(cb);
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cb;
    {
        vkq::Guard lock;
        dfn<PFN_vkQueueSubmit>("vkQueueSubmit")(g_queue, 1, &si, VK_NULL_HANDLE);
        dfn<PFN_vkQueueWaitIdle>("vkQueueWaitIdle")(g_queue);
    }
    dfn<PFN_vkDestroyCommandPool>("vkDestroyCommandPool")(g_dev, pool, nullptr);
}

bool ensureStaging(size_t eyeBytes) {
    if (g_staging && eyeBytes <= g_eyeBytes) return true;
    // (re)create: neutral + ring of eye patterns
    size_t size = size_t(kNeutralTexels) * kNeutralTexels + eyeBytes * kRing;
    VkBufferCreateInfo bc{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bc.size = size;
    bc.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    VkBuffer buf;
    if (dfn<PFN_vkCreateBuffer>("vkCreateBuffer")(g_dev, &bc, nullptr, &buf) != VK_SUCCESS) return false;
    VkMemoryRequirements mr;
    dfn<PFN_vkGetBufferMemoryRequirements>("vkGetBufferMemoryRequirements")(g_dev, buf, &mr);
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.allocationSize = mr.size;
    ai.memoryTypeIndex = memoryType(mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, g_gmp);
    VkDeviceMemory mem;
    if (ai.memoryTypeIndex == UINT32_MAX || dfn<PFN_vkAllocateMemory>("vkAllocateMemory")(g_dev, &ai, nullptr, &mem) != VK_SUCCESS) return false;
    dfn<PFN_vkBindBufferMemory>("vkBindBufferMemory")(g_dev, buf, mem, 0);
    void* p;
    dfn<PFN_vkMapMemory>("vkMapMemory")(g_dev, mem, 0, VK_WHOLE_SIZE, 0, &p);
    // The old buffer may still be referenced by in-flight copies; it is leaked (resizes are rare).
    g_staging = buf;
    g_stagingMem = mem;
    g_stagingPtr = static_cast<uint8_t*>(p);
    g_eyeBytes = eyeBytes;
    memset(g_stagingPtr, rateCode(0, 0), size_t(kNeutralTexels) * kNeutralTexels);
    return true;
}

// Called with g_lock held, from framebuffer creation.
VkImageView viewFor(uint32_t fbW, uint32_t fbH) {
    if (!g_neutral.view) {
        if (!ensureStaging(0) || !createRateImage(g_neutral, kNeutralTexels, kNeutralTexels)) return VK_NULL_HANDLE;
        uploadNow(g_neutral, 0);
    }
    bool eye = g_eyeW && fbW == g_eyeW && fbH == g_eyeH;  // every mode: the menu switches at runtime (Off = full rate)
    if (g_eyeW && fbW == 2 * g_eyeW && fbH == g_eyeH) {  // single pass (EYE_TARGETS.md: the double-wide holder)
        uint32_t tw = (fbW + g_texel - 1) / g_texel, th = (fbH + g_texel - 1) / g_texel;
        if (g_wide.w != tw || g_wide.h != th) {
            if (!ensureStaging(size_t(tw) * th)) return g_neutral.view;
            RateImage r;
            if (!createRateImage(r, tw, th)) return g_neutral.view;
            g_wide = r;
            uint8_t* dst = g_stagingPtr + size_t(kNeutralTexels) * kNeutralTexels;
            memset(dst, rateCode(0, 0), size_t(tw) * th);
            uploadNow(g_wide, size_t(kNeutralTexels) * kNeutralTexels);
            g_wideContent[0] = g_wideContent[1] = {};
            VRLOG("fov: double-wide shading-rate image %ux%u (texel %u) for %ux%u single-pass targets", tw, th, g_texel, fbW, fbH);
        }
        return g_wide.view;
    }
    if (!eye) return g_neutral.view;
    uint32_t tw = (fbW + g_texel - 1) / g_texel, th = (fbH + g_texel - 1) / g_texel;
    if (g_eye.w != tw || g_eye.h != th) {
        if (!ensureStaging(size_t(tw) * th)) return g_neutral.view;
        RateImage r;
        if (!createRateImage(r, tw, th)) return g_neutral.view;
        g_eye = r;  // a previous eye image stays alive for framebuffers that still use it
        uint8_t* dst = g_stagingPtr + size_t(kNeutralTexels) * kNeutralTexels;
        fillPattern(dst, tw, th, 0.5f, 0.5f, 1.0f, 1.0f, g_mode.load() == Mode::Off);
        uploadNow(g_eye, size_t(kNeutralTexels) * kNeutralTexels);
        g_content = {};
        VRLOG("fov: eye shading-rate image %ux%u (texel %u) for %ux%u scene targets", tw, th, g_texel, fbW, fbH);
    }
    return g_eye.view;
}

bool isScenePass(const VkSubpassDescription2& s) {
    return s.pDepthStencilAttachment && s.pDepthStencilAttachment->attachment != VK_ATTACHMENT_UNUSED && s.colorAttachmentCount > 0;
}

VkResult createPass2(VkDevice d, const VkRenderPassCreateInfo2* ci, const VkAllocationCallbacks* a, VkRenderPass* out) {
    uint32_t mask = 0;
    for (uint32_t i = 0; i < ci->subpassCount && i < 32; ++i)
        if (isScenePass(ci->pSubpasses[i])) mask |= 1u << i;
    if (!g_active || !mask) return realCreateRenderPass2(d, ci, a, out);
    std::vector<VkAttachmentDescription2> att(ci->pAttachments, ci->pAttachments + ci->attachmentCount);
    const uint32_t idx = uint32_t(att.size());
    VkAttachmentDescription2 fsr{VK_STRUCTURE_TYPE_ATTACHMENT_DESCRIPTION_2};
    fsr.format = VK_FORMAT_R8_UINT;
    fsr.samples = VK_SAMPLE_COUNT_1_BIT;
    fsr.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    fsr.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    fsr.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    fsr.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    fsr.initialLayout = fsr.finalLayout = VK_IMAGE_LAYOUT_FRAGMENT_SHADING_RATE_ATTACHMENT_OPTIMAL_KHR;
    att.push_back(fsr);
    std::vector<VkSubpassDescription2> sub(ci->pSubpasses, ci->pSubpasses + ci->subpassCount);
    VkAttachmentReference2 ref{VK_STRUCTURE_TYPE_ATTACHMENT_REFERENCE_2};
    ref.attachment = idx;
    ref.layout = VK_IMAGE_LAYOUT_FRAGMENT_SHADING_RATE_ATTACHMENT_OPTIMAL_KHR;
    std::vector<VkFragmentShadingRateAttachmentInfoKHR> info(sub.size());
    for (size_t i = 0; i < sub.size(); ++i) {
        if (!(mask & (1u << i))) continue;
        info[i] = {VK_STRUCTURE_TYPE_FRAGMENT_SHADING_RATE_ATTACHMENT_INFO_KHR};
        info[i].pNext = sub[i].pNext;
        info[i].pFragmentShadingRateAttachment = &ref;
        info[i].shadingRateAttachmentTexelSize = {g_texel, g_texel};
        sub[i].pNext = &info[i];
    }
    VkRenderPassCreateInfo2 c = *ci;
    c.attachmentCount = uint32_t(att.size());
    c.pAttachments = att.data();
    c.pSubpasses = sub.data();
    VkResult r = realCreateRenderPass2(d, &c, a, out);
    if (r == VK_SUCCESS) {
        std::lock_guard<std::mutex> l(g_lock);
        g_passes[*out] = {idx, mask};
        static int logged;
        if (logged++ < 8) VRLOG("fov: scene render pass %p gets shading-rate attachment %u (subpasses %x)", (void*)*out, idx, mask);
    }
    return r;
}

VKAPI_ATTR VkResult VKAPI_CALL hookCreateRenderPass2(VkDevice d, const VkRenderPassCreateInfo2* ci, const VkAllocationCallbacks* a,
                                                     VkRenderPass* out) {
    return createPass2(d, ci, a, out);
}

// Version-1 render passes are converted to version 2 when they are scene passes (only v2 can carry the
// shading-rate attachment); others go to the original call unchanged.
VKAPI_ATTR VkResult VKAPI_CALL hookCreateRenderPass(VkDevice d, const VkRenderPassCreateInfo* ci, const VkAllocationCallbacks* a,
                                                    VkRenderPass* out) {
    bool scene = false;
    for (uint32_t i = 0; i < ci->subpassCount; ++i)
        scene |= ci->pSubpasses[i].pDepthStencilAttachment && ci->pSubpasses[i].pDepthStencilAttachment->attachment != VK_ATTACHMENT_UNUSED &&
                 ci->pSubpasses[i].colorAttachmentCount > 0;
    if (!g_active || !scene || !realCreateRenderPass2) return realCreateRenderPass(d, ci, a, out);
    const VkRenderPassMultiviewCreateInfo* mv = nullptr;
    const VkRenderPassInputAttachmentAspectCreateInfo* asp = nullptr;
    for (auto* s = static_cast<const VkBaseInStructure*>(ci->pNext); s; s = s->pNext) {
        if (s->sType == VK_STRUCTURE_TYPE_RENDER_PASS_MULTIVIEW_CREATE_INFO) mv = reinterpret_cast<const VkRenderPassMultiviewCreateInfo*>(s);
        else if (s->sType == VK_STRUCTURE_TYPE_RENDER_PASS_INPUT_ATTACHMENT_ASPECT_CREATE_INFO)
            asp = reinterpret_cast<const VkRenderPassInputAttachmentAspectCreateInfo*>(s);
        else return realCreateRenderPass(d, ci, a, out);  // unknown extension struct: leave the pass alone
    }
    std::vector<VkAttachmentDescription2> att(ci->attachmentCount);
    for (uint32_t i = 0; i < ci->attachmentCount; ++i) {
        const auto& s = ci->pAttachments[i];
        att[i] = {VK_STRUCTURE_TYPE_ATTACHMENT_DESCRIPTION_2, nullptr, s.flags, s.format, s.samples, s.loadOp, s.storeOp,
                  s.stencilLoadOp, s.stencilStoreOp, s.initialLayout, s.finalLayout};
    }
    // Attachment references: one flat pool so pointers stay valid.
    size_t refCount = 0;
    for (uint32_t i = 0; i < ci->subpassCount; ++i) {
        const auto& s = ci->pSubpasses[i];
        refCount += s.inputAttachmentCount + s.colorAttachmentCount * 2 + 1;
    }
    std::vector<VkAttachmentReference2> refs;
    refs.reserve(refCount);
    auto conv = [&](const VkAttachmentReference& r, VkImageAspectFlags aspect) {
        refs.push_back({VK_STRUCTURE_TYPE_ATTACHMENT_REFERENCE_2, nullptr, r.attachment, r.layout, aspect});
        return &refs.back();
    };
    std::vector<VkSubpassDescription2> sub(ci->subpassCount);
    for (uint32_t i = 0; i < ci->subpassCount; ++i) {
        const auto& s = ci->pSubpasses[i];
        auto& o = sub[i];
        o = {VK_STRUCTURE_TYPE_SUBPASS_DESCRIPTION_2};
        o.flags = s.flags;
        o.pipelineBindPoint = s.pipelineBindPoint;
        o.viewMask = mv && i < mv->subpassCount ? mv->pViewMasks[i] : 0;
        o.inputAttachmentCount = s.inputAttachmentCount;
        if (s.inputAttachmentCount) {
            o.pInputAttachments = refs.data() + refs.size();
            for (uint32_t k = 0; k < s.inputAttachmentCount; ++k) {
                VkImageAspectFlags aspect = VK_IMAGE_ASPECT_COLOR_BIT;
                if (asp)
                    for (uint32_t q = 0; q < asp->aspectReferenceCount; ++q)
                        if (asp->pAspectReferences[q].subpass == i && asp->pAspectReferences[q].inputAttachmentIndex == k)
                            aspect = asp->pAspectReferences[q].aspectMask;
                conv(s.pInputAttachments[k], aspect);
            }
        }
        o.colorAttachmentCount = s.colorAttachmentCount;
        if (s.colorAttachmentCount) {
            o.pColorAttachments = refs.data() + refs.size();
            for (uint32_t k = 0; k < s.colorAttachmentCount; ++k) conv(s.pColorAttachments[k], 0);
            if (s.pResolveAttachments) {
                o.pResolveAttachments = refs.data() + refs.size();
                for (uint32_t k = 0; k < s.colorAttachmentCount; ++k) conv(s.pResolveAttachments[k], 0);
            }
        }
        if (s.pDepthStencilAttachment) o.pDepthStencilAttachment = conv(*s.pDepthStencilAttachment, 0);
        o.preserveAttachmentCount = s.preserveAttachmentCount;
        o.pPreserveAttachments = s.pPreserveAttachments;
    }
    std::vector<VkSubpassDependency2> dep(ci->dependencyCount);
    for (uint32_t i = 0; i < ci->dependencyCount; ++i) {
        const auto& s = ci->pDependencies[i];
        dep[i] = {VK_STRUCTURE_TYPE_SUBPASS_DEPENDENCY_2, nullptr, s.srcSubpass, s.dstSubpass, s.srcStageMask, s.dstStageMask,
                  s.srcAccessMask, s.dstAccessMask, s.dependencyFlags, mv && i < mv->dependencyCount ? mv->pViewOffsets[i] : 0};
    }
    VkRenderPassCreateInfo2 c{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO_2};
    c.flags = ci->flags;
    c.attachmentCount = uint32_t(att.size());
    c.pAttachments = att.data();
    c.subpassCount = uint32_t(sub.size());
    c.pSubpasses = sub.data();
    c.dependencyCount = uint32_t(dep.size());
    c.pDependencies = dep.data();
    if (mv) {
        c.correlatedViewMaskCount = mv->correlationMaskCount;
        c.pCorrelatedViewMasks = mv->pCorrelationMasks;
    }
    return createPass2(d, &c, a, out);
}

VKAPI_ATTR VkResult VKAPI_CALL hookCreateFramebuffer(VkDevice d, const VkFramebufferCreateInfo* ci, const VkAllocationCallbacks* a,
                                                     VkFramebuffer* out) {
    PassInfo pi;
    VkImageView view;
    {
        std::lock_guard<std::mutex> l(g_lock);
        auto it = g_passes.find(ci->renderPass);
        if (it == g_passes.end()) return realCreateFramebuffer(d, ci, a, out);
        pi = it->second;
        view = viewFor(ci->width, ci->height);
        static int logged;
        if (g_trace && logged++ < 40)
            VRLOG("fov: framebuffer %ux%u (layers %u, %u attachments%s) on scene pass %p gets the %s image", ci->width, ci->height, ci->layers,
                  ci->attachmentCount, (ci->flags & VK_FRAMEBUFFER_CREATE_IMAGELESS_BIT) ? ", imageless" : "", reinterpret_cast<void*>(ci->renderPass),
                  view && view == g_eye.view ? "eye" : "neutral");
    }
    VkFramebufferCreateInfo c = *ci;
    std::vector<VkImageView> views;
    std::vector<VkFramebufferAttachmentImageInfo> infos;
    VkFramebufferAttachmentsCreateInfo ai{};
    VkFormat fmt = VK_FORMAT_R8_UINT;
    bool imageless = (ci->flags & VK_FRAMEBUFFER_CREATE_IMAGELESS_BIT) != 0;
    if (imageless) {
        for (auto* s = static_cast<const VkBaseInStructure*>(ci->pNext); s; s = s->pNext)
            if (s->sType == VK_STRUCTURE_TYPE_FRAMEBUFFER_ATTACHMENTS_CREATE_INFO) {
                ai = *reinterpret_cast<const VkFramebufferAttachmentsCreateInfo*>(s);
                infos.assign(ai.pAttachmentImageInfos, ai.pAttachmentImageInfos + ai.attachmentImageInfoCount);
            }
        VkFramebufferAttachmentImageInfo f{VK_STRUCTURE_TYPE_FRAMEBUFFER_ATTACHMENT_IMAGE_INFO};
        f.usage = VK_IMAGE_USAGE_FRAGMENT_SHADING_RATE_ATTACHMENT_BIT_KHR | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        f.width = (ci->width + g_texel - 1) / g_texel;
        f.height = (ci->height + g_texel - 1) / g_texel;
        f.layerCount = 1;
        f.viewFormatCount = 1;
        f.pViewFormats = &fmt;
        infos.push_back(f);
        ai.attachmentImageInfoCount = uint32_t(infos.size());
        ai.pAttachmentImageInfos = infos.data();
        ai.pNext = nullptr;
        c.pNext = &ai;  // the only struct an imageless framebuffer chains
    } else {
        if (!view) return realCreateFramebuffer(d, ci, a, out);
        views.assign(ci->pAttachments, ci->pAttachments + ci->attachmentCount);
        views.push_back(view);
        c.pAttachments = views.data();
    }
    c.attachmentCount = ci->attachmentCount + 1;
    VkResult r = realCreateFramebuffer(d, &c, a, out);
    if (r == VK_SUCCESS && imageless) {
        std::lock_guard<std::mutex> l(g_lock);
        g_imageless.insert(*out);
    } else if (r == VK_SUCCESS && view && view == g_eye.view) {
        std::lock_guard<std::mutex> l(g_lock);
        g_eyeFbs.insert(*out);
    }
    return r;
}

void upload(VkCommandBuffer cb, const Content& c);
// Before a pass on an eye framebuffer (outside any render pass): the pattern the current eye wants.
void beforeEyePass(VkCommandBuffer cb) {
    std::lock_guard<std::mutex> l(g_lock);
    if (g_haveWant && g_eye.image && !(g_want == g_content)) upload(cb, g_want);
}

// Imageless framebuffers name their views at begin: append ours.
const VkRenderPassBeginInfo* patchBegin(VkCommandBuffer cb, const VkRenderPassBeginInfo* bi, VkRenderPassBeginInfo& copy,
                                        VkRenderPassAttachmentBeginInfo& ab, std::vector<VkImageView>& views) {
    VkImageView view;
    bool eye;
    {
        std::lock_guard<std::mutex> l(g_lock);
        if (!g_imageless.count(bi->framebuffer)) eye = g_eyeFbs.count(bi->framebuffer) != 0, view = VK_NULL_HANDLE;
        else {
            view = viewFor(bi->renderArea.offset.x + bi->renderArea.extent.width, bi->renderArea.offset.y + bi->renderArea.extent.height);
            eye = view && view == g_eye.view;
        }
    }
    if (eye) beforeEyePass(cb);
    if (g_trace) {
        static volatile LONG n[2];
        static ULONGLONG last;
        InterlockedIncrement(&n[eye ? 1 : 0]);
        const ULONGLONG now = GetTickCount64();
        if (now - last > 5000) {
            last = now;
            VRLOG("fov: passes begun in 5 s: %ld on eye framebuffers, %ld others (area %ux%u)", InterlockedExchange(&n[1], 0),
                  InterlockedExchange(&n[0], 0), bi->renderArea.extent.width, bi->renderArea.extent.height);
        }
    }
    if (!view) return bi;
    copy = *bi;
    for (auto* s = static_cast<const VkBaseInStructure*>(bi->pNext); s; s = s->pNext)
        if (s->sType == VK_STRUCTURE_TYPE_RENDER_PASS_ATTACHMENT_BEGIN_INFO) {
            ab = *reinterpret_cast<const VkRenderPassAttachmentBeginInfo*>(s);
            views.assign(ab.pAttachments, ab.pAttachments + ab.attachmentCount);
            views.push_back(view);
            ab.attachmentCount = uint32_t(views.size());
            ab.pAttachments = views.data();
            ab.pNext = nullptr;
            copy.pNext = &ab;
            return &copy;
        }
    return bi;
}
VKAPI_ATTR void VKAPI_CALL hookCmdBeginRenderPass(VkCommandBuffer cb, const VkRenderPassBeginInfo* bi, VkSubpassContents c) {
    VkRenderPassBeginInfo copy;
    VkRenderPassAttachmentBeginInfo ab;
    std::vector<VkImageView> views;
    realCmdBeginRenderPass(cb, patchBegin(cb, bi, copy, ab, views), c);
}
VKAPI_ATTR void VKAPI_CALL hookCmdBeginRenderPass2(VkCommandBuffer cb, const VkRenderPassBeginInfo* bi, const VkSubpassBeginInfo* si) {
    VkRenderPassBeginInfo copy;
    VkRenderPassAttachmentBeginInfo ab;
    std::vector<VkImageView> views;
    realCmdBeginRenderPass2(cb, patchBegin(cb, bi, copy, ab, views), si);
}

// Every graphics pipeline: the attachment rate combines with the pipeline/primitive rate (MAX: the coarser wins). The
// engine creates pipelines against compatible render passes that are not always the patched handles, and a pipeline
// without this state keeps the attachment out (KEEP). In a pass without the attachment the attachment rate is 1x1,
// so MAX changes nothing there.
VKAPI_ATTR VkResult VKAPI_CALL hookCreateGraphicsPipelines(VkDevice d, VkPipelineCache cache, uint32_t n,
                                                           const VkGraphicsPipelineCreateInfo* ci, const VkAllocationCallbacks* a,
                                                           VkPipeline* out) {
    std::vector<VkGraphicsPipelineCreateInfo> c(ci, ci + n);
    std::vector<VkPipelineFragmentShadingRateStateCreateInfoKHR> fsr(n);
    bool any = false;
    {
        std::lock_guard<std::mutex> l(g_lock);
        static uint32_t total, scene;
        for (uint32_t i = 0; i < n; ++i) {
            auto it = g_passes.find(ci[i].renderPass);
            ++total;
            if (it != g_passes.end() && (it->second.subpassMask & (1u << ci[i].subpass))) ++scene;
            if (g_trace && (total & (total - 1)) == 0 && total >= 64)
                VRLOG("fov: %u graphics pipelines combine the attachment rate (%u created on patched scene passes)", total, scene);
            VkPipelineFragmentShadingRateStateCreateInfoKHR* existing = nullptr;
            for (auto* s = static_cast<const VkBaseInStructure*>(ci[i].pNext); s; s = s->pNext)
                if (s->sType == VK_STRUCTURE_TYPE_PIPELINE_FRAGMENT_SHADING_RATE_STATE_CREATE_INFO_KHR)
                    existing = reinterpret_cast<VkPipelineFragmentShadingRateStateCreateInfoKHR*>(const_cast<VkBaseInStructure*>(s));
            if (existing) {
                existing->combinerOps[1] = g_combine;  // the caller's struct; it only lives for this call
            } else {
                fsr[i] = {VK_STRUCTURE_TYPE_PIPELINE_FRAGMENT_SHADING_RATE_STATE_CREATE_INFO_KHR};
                fsr[i].pNext = ci[i].pNext;
                fsr[i].fragmentSize = {1, 1};
                fsr[i].combinerOps[0] = VK_FRAGMENT_SHADING_RATE_COMBINER_OP_KEEP_KHR;
                fsr[i].combinerOps[1] = g_combine;
                c[i].pNext = &fsr[i];
            }
            any = true;
        }
    }
    const VkResult r = realCreateGraphicsPipelines(d, cache, n, any ? c.data() : ci, a, out);
    if (r == VK_SUCCESS || r == VK_PIPELINE_COMPILE_REQUIRED) {
        uint32_t dyn = 0;
        AcquireSRWLockExclusive(&g_dynLock);
        for (uint32_t i = 0; i < n; ++i) {
            const VkPipelineDynamicStateCreateInfo* ds = ci[i].pDynamicState;
            for (uint32_t k = 0; ds && k < ds->dynamicStateCount; ++k)
                if (ds->pDynamicStates[k] == VK_DYNAMIC_STATE_FRAGMENT_SHADING_RATE_KHR && out[i]) g_dynFsr.insert(out[i]), ++dyn;
        }
        const size_t total = g_dynFsr.size();
        ReleaseSRWLockExclusive(&g_dynLock);
        static size_t logged;
        if (dyn && total >= logged * 2 + 16) logged = total, VRLOG("fov: %zu pipelines take the shading rate as dynamic state (set at bind)", total);
    }
    return r;
}

VKAPI_ATTR void VKAPI_CALL hookCmdBindPipeline(VkCommandBuffer cb, VkPipelineBindPoint bp, VkPipeline p) {
    realCmdBindPipeline(cb, bp, p);
    if (bp != VK_PIPELINE_BIND_POINT_GRAPHICS || !realCmdSetFsr) return;
    AcquireSRWLockShared(&g_dynLock);
    const bool dyn = g_dynFsr.count(p) != 0;
    ReleaseSRWLockShared(&g_dynLock);
    if (!dyn) return;
    const VkFragmentShadingRateCombinerOpKHR o[2] = {t_fsrOp0, g_combine};
    realCmdSetFsr(cb, &t_fsrSize, o);
}

VKAPI_ATTR void VKAPI_CALL hookCmdSetFsr(VkCommandBuffer cb, const VkExtent2D* size, const VkFragmentShadingRateCombinerOpKHR ops[2]) {
    if (size) t_fsrSize = *size;
    t_fsrOp0 = ops[0];
    VkFragmentShadingRateCombinerOpKHR o[2] = {ops[0], g_combine};
    realCmdSetFsr(cb, size, o);
}
}  // namespace

void decide() {
    auto& rt = xr::runtime();
    std::string name = rt.systemName();
    std::transform(name.begin(), name.end(), name.begin(), [](unsigned char ch) { return char(std::tolower(ch)); });
    const bool frame = name.find("frame") != std::string::npos;
    // Lab: lab_fov_gaze=1 simulates an eye tracker whose gaze rests up and to the left (tangents -0.4, +0.3), so captures
    // show the full-rate region off the lens centre.
    g_labGaze = se4::options::get("lab_fov_gaze", 0) != 0;
    g_trace = se4::options::get("lab_fov_trace", 0) != 0;
    g_allow4x4 = se4::options::get("foveation_coarse", 2) >= 4;
    const bool gaze = rt.eyeGazeSupported() || g_labGaze;
    g_gaze = gaze;
    // Default: eye-tracked on any headset whose runtime offers eye gaze (Steam Frame, Quest Pro, Pimax Crystal, Varjo,
    // Vive Pro Eye ...), off otherwise. Fixed is the choice for headsets without eye tracking. The menu changes it
    // (src/se4/gfx_menu.cpp, foveation=); option foveation (-1 auto, 0 off, 1 fixed, 2 eye-tracked) is the start.
    int opt = se4::options::has("foveation") ? se4::options::get("foveation", -1) : g_startChoice;
    if (opt < 0) g_mode = gaze ? Mode::Dynamic : Mode::Off;
    else if (opt == 2) g_mode = gaze ? Mode::Dynamic : Mode::Fixed;
    else g_mode = opt == 1 ? Mode::Fixed : Mode::Off;
    g_inner = std::clamp(se4::options::get("foveation_inner", 20), 5, 90) / 100.0f;
    g_outer = std::clamp(se4::options::get("foveation_outer", 40), 10, 120) / 100.0f;
    g_fixedInner = std::clamp(se4::options::get("foveation_fixed_inner", 30), 5, 90) / 100.0f;
    g_fixedOuter = std::clamp(se4::options::get("foveation_fixed_outer", 50), 10, 120) / 100.0f;
    VRLOG("fov: start %s (headset '%s', Steam Frame %s, eye gaze %s%s; radii eye-tracked %.2f/%.2f, fixed %.2f/%.2f, coarsest %s)",
          g_mode.load() == Mode::Dynamic ? "eye-tracked" : g_mode.load() == Mode::Fixed ? "fixed" : "off", rt.systemName().c_str(), frame ? "yes" : "no",
          gaze ? "yes" : "no", g_labGaze ? ", simulated (lab)" : "", g_inner, g_outer, g_fixedInner, g_fixedOuter, g_allow4x4 ? "4x4" : "2x2");
}

Mode mode() { return g_active ? g_mode.load() : Mode::Off; }
bool available() { return g_active; }
void setStartChoice(int m) { g_startChoice = std::clamp(m, -1, 2); }
bool gazeSupported() { return g_gaze; }
void setMode(Mode m) {
    if (m == Mode::Dynamic && !g_gaze) m = Mode::Fixed;
    if (g_mode.exchange(m) != m) VRLOG("fov: mode %s", m == Mode::Dynamic ? "eye-tracked" : m == Mode::Fixed ? "fixed" : "off");
}

bool patchDeviceCreate(VkInstance inst, PFN_vkGetInstanceProcAddr gipa, VkPhysicalDevice pd, VkDeviceCreateInfo& ci, void* storage,
                       size_t storageSize) {
    // Every mode, Off included, when the device can: the menu switches modes at runtime (Off is a full-rate pattern).
    // Option foveation_device=0 leaves the device and its passes untouched (lab).
    if (!se4::options::get("foveation_device", 1) || storageSize < sizeof(VkPhysicalDeviceFragmentShadingRateFeaturesKHR)) return false;
    bool ext = false;
    for (uint32_t i = 0; i < ci.enabledExtensionCount; ++i)
        ext |= !strcmp(ci.ppEnabledExtensionNames[i], VK_KHR_FRAGMENT_SHADING_RATE_EXTENSION_NAME);
    auto gf2 = reinterpret_cast<PFN_vkGetPhysicalDeviceFeatures2>(gipa(inst, "vkGetPhysicalDeviceFeatures2"));
    VkPhysicalDeviceFragmentShadingRateFeaturesKHR sup{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_SHADING_RATE_FEATURES_KHR};
    VkPhysicalDeviceFeatures2 f2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &sup};
    if (gf2) gf2(pd, &f2);
    if (!ext || !sup.attachmentFragmentShadingRate) {
        VRLOG("fov: unavailable (VK_KHR_fragment_shading_rate %s, attachment rates %s) - foveation off", ext ? "on" : "off",
              sup.attachmentFragmentShadingRate ? "yes" : "no");
        g_mode = Mode::Off;
        return false;
    }
    for (auto* s = static_cast<const VkBaseInStructure*>(ci.pNext); s; s = s->pNext)
        if (s->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_SHADING_RATE_FEATURES_KHR) {
            // The game's own struct (it already enables pipeline rates for its VRS options).
            reinterpret_cast<VkPhysicalDeviceFragmentShadingRateFeaturesKHR*>(const_cast<VkBaseInStructure*>(s))->attachmentFragmentShadingRate =
                VK_TRUE;
            g_active = true;
            return true;
        }
    auto* mine = new (storage) VkPhysicalDeviceFragmentShadingRateFeaturesKHR{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_SHADING_RATE_FEATURES_KHR};
    mine->attachmentFragmentShadingRate = VK_TRUE;
    mine->pNext = const_cast<void*>(ci.pNext);
    ci.pNext = mine;
    g_active = true;
    return true;
}

void onDevice(VkInstance inst, PFN_vkGetInstanceProcAddr gipa, PFN_vkGetDeviceProcAddr gdpa, VkPhysicalDevice pd, VkDevice d,
              VkQueue queue, uint32_t family) {
    if (!g_active) return;
    g_dev = d, g_pd = pd, g_queue = queue, g_family = family, g_gdpa = gdpa;
    g_gmp = reinterpret_cast<PFN_vkGetPhysicalDeviceMemoryProperties>(gipa(inst, "vkGetPhysicalDeviceMemoryProperties"));
    auto gp2 = reinterpret_cast<PFN_vkGetPhysicalDeviceProperties2>(gipa(inst, "vkGetPhysicalDeviceProperties2"));
    VkPhysicalDeviceFragmentShadingRatePropertiesKHR fp{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_SHADING_RATE_PROPERTIES_KHR};
    VkPhysicalDeviceProperties2 p2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &fp};
    if (gp2) gp2(pd, &p2);
    g_texel = std::clamp(16u, fp.minFragmentShadingRateAttachmentTexelSize.width, std::max(fp.maxFragmentShadingRateAttachmentTexelSize.width, 1u));
    if (fp.minFragmentShadingRateAttachmentTexelSize.width != fp.minFragmentShadingRateAttachmentTexelSize.height) g_texel = 16;
    g_combine = fp.fragmentShadingRateNonTrivialCombinerOps ? VK_FRAGMENT_SHADING_RATE_COMBINER_OP_MAX_KHR
                                                            : VK_FRAGMENT_SHADING_RATE_COMBINER_OP_REPLACE_KHR;
    auto rates = reinterpret_cast<PFN_vkGetPhysicalDeviceFragmentShadingRatesKHR>(gipa(inst, "vkGetPhysicalDeviceFragmentShadingRatesKHR"));
    if (rates) {
        uint32_t n = 0;
        rates(pd, &n, nullptr);
        std::vector<VkPhysicalDeviceFragmentShadingRateKHR> r(n, {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_SHADING_RATE_KHR});
        rates(pd, &n, r.data());
        for (auto& x : r) g_have4x4 |= x.fragmentSize.width == 4 && x.fragmentSize.height == 4;
    }
    realCreateRenderPass = dfn<PFN_vkCreateRenderPass>("vkCreateRenderPass");
    realCreateRenderPass2 = dfn<PFN_vkCreateRenderPass2>("vkCreateRenderPass2");
    if (!realCreateRenderPass2) realCreateRenderPass2 = dfn<PFN_vkCreateRenderPass2>("vkCreateRenderPass2KHR");
    realCreateFramebuffer = dfn<PFN_vkCreateFramebuffer>("vkCreateFramebuffer");
    realCreateGraphicsPipelines = dfn<PFN_vkCreateGraphicsPipelines>("vkCreateGraphicsPipelines");
    realCmdSetFsr = dfn<PFN_vkCmdSetFragmentShadingRateKHR>("vkCmdSetFragmentShadingRateKHR");
    realCmdBindPipeline = dfn<PFN_vkCmdBindPipeline>("vkCmdBindPipeline");
    realCmdBeginRenderPass = dfn<PFN_vkCmdBeginRenderPass>("vkCmdBeginRenderPass");
    realCmdBeginRenderPass2 = dfn<PFN_vkCmdBeginRenderPass2>("vkCmdBeginRenderPass2");
    if (!realCmdBeginRenderPass2) realCmdBeginRenderPass2 = dfn<PFN_vkCmdBeginRenderPass2>("vkCmdBeginRenderPass2KHR");
    if (!realCreateRenderPass2 || !realCreateFramebuffer || !realCreateGraphicsPipelines) {
        VRLOG("fov: device lacks render pass 2 entry points - foveation off");
        g_active = false;
        return;
    }
    VRLOG("fov: active, texel %u, 4x4 %s, combiner %s", g_texel, g_have4x4 ? "yes" : "no",
          g_combine == VK_FRAGMENT_SHADING_RATE_COMBINER_OP_MAX_KHR ? "max" : "replace");
}

PFN_vkVoidFunction override(const char* name) {
    if (!g_active) return nullptr;
#define OV(n, f) \
    if (!strcmp(name, n)) return reinterpret_cast<PFN_vkVoidFunction>(f);
    OV("vkCreateRenderPass", hookCreateRenderPass)
    OV("vkCreateRenderPass2", hookCreateRenderPass2)
    OV("vkCreateRenderPass2KHR", hookCreateRenderPass2)
    OV("vkCreateFramebuffer", hookCreateFramebuffer)
    OV("vkCreateGraphicsPipelines", hookCreateGraphicsPipelines)
    if (realCmdSetFsr) OV("vkCmdSetFragmentShadingRateKHR", hookCmdSetFsr)
    if (realCmdSetFsr && realCmdBindPipeline) OV("vkCmdBindPipeline", hookCmdBindPipeline)
    if (realCmdBeginRenderPass) OV("vkCmdBeginRenderPass", hookCmdBeginRenderPass)
    if (realCmdBeginRenderPass2) {
        OV("vkCmdBeginRenderPass2", hookCmdBeginRenderPass2)
        OV("vkCmdBeginRenderPass2KHR", hookCmdBeginRenderPass2)
    }
#undef OV
    return nullptr;
}

void setEyeSize(uint32_t w, uint32_t h) {
    g_eyeW = w;
    g_eyeH = h;
}

namespace {
// The pattern centre of `eye` (1/2) in its own image: the gaze point (eye-tracked, when the runtime has a gaze this
// frame) or the lens centre (the projection's optical axis; the eye frusta are asymmetric). False without views.
// Eye-tracked without a gaze this frame (blink, tracking lost) turns `m` into Fixed: the lens centre with the wider radii.
bool centre(int eye, Mode& m, float& cx, float& cy) {
    float l, r, u, d;
    if (!vrdev::eyeFov(eye, l, r, u, d)) return false;
    const float tl = std::tan(l), tr = std::tan(r), tu = std::tan(u), td = std::tan(d);
    float tx = 0, ty = 0;  // the optical axis
    XrPosef gz;
    if (m == Mode::Dynamic && g_labGaze) tx = -0.4f, ty = 0.3f;
    else if (m == Mode::Dynamic && xr::input().gaze(gz)) {
        // gaze direction in VIEW space: -Z of its orientation; both eyes take the same direction (a far fixation)
        const auto& q = gz.orientation;
        const float dx = -(2 * (q.x * q.z + q.w * q.y)), dy = -(2 * (q.y * q.z - q.w * q.x)), dz = -(1 - 2 * (q.x * q.x + q.y * q.y));
        if (dz < -0.1f) tx = dx / -dz, ty = dy / -dz;
        else m = Mode::Fixed;
    } else if (m == Mode::Dynamic) m = Mode::Fixed;
    cx = std::clamp((tx - tl) / (tr - tl), 0.0f, 1.0f);
    cy = std::clamp((tu - ty) / (tu - td), 0.0f, 1.0f);
    return true;
}

void upload(VkCommandBuffer cb, const Content& c) {
    g_rIn = c.mode == Mode::Dynamic ? g_inner : g_fixedInner;
    g_rOut = c.mode == Mode::Dynamic ? g_outer : g_fixedOuter;
    g_ring = (g_ring + 1) % kRing;
    const size_t off = size_t(kNeutralTexels) * kNeutralTexels + g_eyeBytes * g_ring;
    fillPattern(g_stagingPtr + off, g_eye.w, g_eye.h, c.cx, c.cy, c.sx, c.sy, c.mode == Mode::Off);
    copyIn(cb, g_eye, off, false);
    g_content = c;
}

}  // namespace

void beforeEye(int eye, float sx, float sy) {
    if (!g_active || !g_eye.image || (eye != 1 && eye != 2)) return;
    std::lock_guard<std::mutex> l(g_lock);
    Content c;
    c.mode = g_mode.load();
    c.sx = std::clamp(sx, 0.1f, 1.0f), c.sy = std::clamp(sy, 0.1f, 1.0f);
    if (c.mode != Mode::Off && !centre(eye, c.mode, c.cx, c.cy)) return;
    if (c.mode == Mode::Off) c.cx = c.cy = 0.5f, c.sx = c.sy = 1.0f;
    InterlockedIncrement(&g_eyeUploads);
    const bool same = g_haveWant && c == g_want;
    g_want = c, g_haveWant = true;
    if (same) return;
    static int logged;
    if (logged++ < 6)
        VRLOG("fov: eye %d pattern %s centre %.3f %.3f (render %.2f x %.2f of the target)", eye,
              c.mode == Mode::Dynamic ? "eye-tracked" : c.mode == Mode::Fixed ? "fixed" : "off", c.cx, c.cy, c.sx, c.sy);
}

// Once per presented frame. With two-pass stereo beforeEye keeps each eye's pattern; single pass renders both eyes
// in one pass, so one pattern covers both (the average of the two centres keeps both foveae inside).
void record(VkCommandBuffer cb) {
    if (!g_active) return;
    if (g_wide.image) {  // single pass: both halves, each with its own eye's centre
        std::lock_guard<std::mutex> l(g_lock);
        Content c[2];
        bool ok = true;
        for (int e = 0; e < 2; ++e) {
            c[e].mode = g_mode.load();
            c[e].sx = c[e].sy = 1.0f;
            c[e].cx = c[e].cy = 0.5f;
            if (c[e].mode != Mode::Off) ok = ok && centre(e + 1, c[e].mode, c[e].cx, c[e].cy);
        }
        if (ok && !(c[0] == g_wideContent[0] && c[1] == g_wideContent[1])) {
            g_ring = (g_ring + 1) % kRing;
            const size_t off = size_t(kNeutralTexels) * kNeutralTexels + g_eyeBytes * g_ring;
            const uint32_t half = g_wide.w / 2;
            for (int e = 0; e < 2; ++e) {
                const float rIn = c[e].mode == Mode::Dynamic ? g_inner : g_fixedInner, rOut = c[e].mode == Mode::Dynamic ? g_outer : g_fixedOuter;
                fillRegion(g_stagingPtr + off, g_wide.w, e ? half : 0, e ? g_wide.w : half, g_wide.h, c[e].cx, c[e].cy, 1.0f, 1.0f,
                           c[e].mode == Mode::Off, rIn, rOut);
            }
            copyIn(cb, g_wide, off, false);
            g_wideContent[0] = c[0], g_wideContent[1] = c[1];
            static int logged;
            if (logged++ < 4)
                VRLOG("fov: single-pass pattern %s, centres %.3f %.3f | %.3f %.3f", c[0].mode == Mode::Dynamic ? "eye-tracked" : c[0].mode == Mode::Fixed ? "fixed" : "off",
                      c[0].cx, c[0].cy, c[1].cx, c[1].cy);
        }
    }
    if (InterlockedExchange(&g_eyeUploads, 0) || !g_eye.image) return;
    std::lock_guard<std::mutex> l(g_lock);
    Content c;
    c.mode = g_mode.load();
    c.sx = c.sy = 1.0f;
    c.cx = c.cy = 0.5f;
    if (c.mode != Mode::Off) {
        float x1, y1, x2, y2;
        Mode m2 = c.mode;
        if (!centre(1, c.mode, x1, y1) || !centre(2, m2, x2, y2)) return;
        c.cx = 0.5f * (x1 + x2), c.cy = 0.5f * (y1 + y2);
    }
    g_haveWant = false;
    if (!(c == g_content)) upload(cb, c);
}
}  // namespace fov
