// Foveated rendering via VK_KHR_fragment_shading_rate attachments. Scene render passes (a depth attachment
// and at least one colour attachment) get one extra attachment: an R8_UINT shading-rate image. Framebuffers
// of the eye render size use the foveation pattern; every other framebuffer of such a pass uses a neutral
// (full-rate) image. Pipelines of those passes combine the engine's own per-draw rate with the attachment
// (coarser wins), so the engine's VRS menu options keep working.
#include "vk/foveation.hpp"
#include "se4/log.hpp"
#include "se4/options.hpp"
#include "vk/queue_lock.hpp"
#include "xr/input.hpp"
#include "xr/runtime.hpp"
#include <windows.h>
#include <algorithm>
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
Mode g_mode = Mode::Off;
bool g_active;  // device supports it and the device was patched
VkDevice g_dev;
VkPhysicalDevice g_pd;
VkQueue g_queue;
uint32_t g_family;
PFN_vkGetDeviceProcAddr g_gdpa;
uint32_t g_texel = 16;
bool g_have4x4;
VkFragmentShadingRateCombinerOpKHR g_combine = VK_FRAGMENT_SHADING_RATE_COMBINER_OP_REPLACE_KHR;
float g_inner = 0.25f, g_outer = 0.45f;
uint32_t g_eyeW, g_eyeH;

PFN_vkCreateRenderPass realCreateRenderPass;
PFN_vkCreateRenderPass2 realCreateRenderPass2;
PFN_vkCreateFramebuffer realCreateFramebuffer;
PFN_vkCreateGraphicsPipelines realCreateGraphicsPipelines;
PFN_vkCmdSetFragmentShadingRateKHR realCmdSetFsr;
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
// Upload staging (host visible): neutral pattern at offset 0, then a ring of eye patterns.
VkBuffer g_staging;
VkDeviceMemory g_stagingMem;
uint8_t* g_stagingPtr;
constexpr uint32_t kNeutralTexels = 512;  // covers framebuffers up to 512 * texel (8192 px at 16)
constexpr uint32_t kRing = 4;
size_t g_eyeBytes;
uint32_t g_ring;
bool g_eyeDirty = true;
float g_lastCx = -1, g_lastCy = -1;

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
void fillPattern(uint8_t* dst, uint32_t w, uint32_t h, float cx, float cy) {
    const uint8_t full = rateCode(0, 0), half = rateCode(1, 1), quarter = g_have4x4 ? rateCode(2, 2) : half;
    const float aspect = float(w) / float(h);
    for (uint32_t y = 0; y < h; ++y)
        for (uint32_t x = 0; x < w; ++x) {
            float dx = ((x + 0.5f) / w - cx) * aspect, dy = (y + 0.5f) / h - cy;
            float d = std::sqrt(dx * dx + dy * dy);
            dst[y * w + x] = d < g_inner ? full : d < g_outer ? half : quarter;
        }
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
    bool eye = g_mode != Mode::Off && g_eyeW && fbW == g_eyeW && fbH == g_eyeH;
    if (!eye) return g_neutral.view;
    uint32_t tw = (fbW + g_texel - 1) / g_texel, th = (fbH + g_texel - 1) / g_texel;
    if (g_eye.w != tw || g_eye.h != th) {
        if (!ensureStaging(size_t(tw) * th)) return g_neutral.view;
        RateImage r;
        if (!createRateImage(r, tw, th)) return g_neutral.view;
        g_eye = r;  // a previous eye image stays alive for framebuffers that still use it
        uint8_t* dst = g_stagingPtr + size_t(kNeutralTexels) * kNeutralTexels;
        fillPattern(dst, tw, th, 0.5f, 0.5f);
        uploadNow(g_eye, size_t(kNeutralTexels) * kNeutralTexels);
        g_eyeDirty = true;
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
    }
    return r;
}

// Imageless framebuffers name their views at begin: append ours.
const VkRenderPassBeginInfo* patchBegin(const VkRenderPassBeginInfo* bi, VkRenderPassBeginInfo& copy,
                                        VkRenderPassAttachmentBeginInfo& ab, std::vector<VkImageView>& views) {
    VkImageView view;
    {
        std::lock_guard<std::mutex> l(g_lock);
        if (!g_imageless.count(bi->framebuffer)) return bi;
        view = viewFor(bi->renderArea.offset.x + bi->renderArea.extent.width, bi->renderArea.offset.y + bi->renderArea.extent.height);
    }
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
    realCmdBeginRenderPass(cb, patchBegin(bi, copy, ab, views), c);
}
VKAPI_ATTR void VKAPI_CALL hookCmdBeginRenderPass2(VkCommandBuffer cb, const VkRenderPassBeginInfo* bi, const VkSubpassBeginInfo* si) {
    VkRenderPassBeginInfo copy;
    VkRenderPassAttachmentBeginInfo ab;
    std::vector<VkImageView> views;
    realCmdBeginRenderPass2(cb, patchBegin(bi, copy, ab, views), si);
}

// Pipelines of foveated subpasses: the attachment rate combines with the pipeline/primitive rate.
VKAPI_ATTR VkResult VKAPI_CALL hookCreateGraphicsPipelines(VkDevice d, VkPipelineCache cache, uint32_t n,
                                                           const VkGraphicsPipelineCreateInfo* ci, const VkAllocationCallbacks* a,
                                                           VkPipeline* out) {
    std::vector<VkGraphicsPipelineCreateInfo> c(ci, ci + n);
    std::vector<VkPipelineFragmentShadingRateStateCreateInfoKHR> fsr(n);
    bool any = false;
    {
        std::lock_guard<std::mutex> l(g_lock);
        for (uint32_t i = 0; i < n; ++i) {
            auto it = g_passes.find(ci[i].renderPass);
            if (it == g_passes.end() || !(it->second.subpassMask & (1u << ci[i].subpass))) continue;
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
    return realCreateGraphicsPipelines(d, cache, n, any ? c.data() : ci, a, out);
}

VKAPI_ATTR void VKAPI_CALL hookCmdSetFsr(VkCommandBuffer cb, const VkExtent2D* size, const VkFragmentShadingRateCombinerOpKHR ops[2]) {
    VkFragmentShadingRateCombinerOpKHR o[2] = {ops[0], g_combine};
    realCmdSetFsr(cb, size, o);
}
}  // namespace

void decide() {
    auto& rt = xr::runtime();
    std::string name = rt.systemName();
    std::transform(name.begin(), name.end(), name.begin(), [](unsigned char ch) { return char(std::tolower(ch)); });
    const bool frame = name.find("frame") != std::string::npos;
    const bool gaze = rt.eyeGazeSupported();
    int opt = se4::options::get("foveation", -1);  // -1 auto, 0 off, 1 fixed, 2 dynamic
    if (opt < 0) g_mode = frame && gaze ? Mode::Dynamic : Mode::Off;
    else if (opt == 2) g_mode = gaze ? Mode::Dynamic : Mode::Fixed;
    else g_mode = opt == 1 ? Mode::Fixed : Mode::Off;
    g_inner = std::clamp(se4::options::get("foveation_inner", g_mode == Mode::Dynamic ? 18 : 30), 5, 90) / 100.0f;
    g_outer = std::clamp(se4::options::get("foveation_outer", g_mode == Mode::Dynamic ? 35 : 50), 10, 120) / 100.0f;
    VRLOG("fov: %s (headset '%s', Steam Frame %s, eye gaze %s; inner %.2f outer %.2f)",
          g_mode == Mode::Dynamic ? "dynamic (eye-tracked)" : g_mode == Mode::Fixed ? "fixed" : "off", rt.systemName().c_str(),
          frame ? "yes" : "no", gaze ? "yes" : "no", g_inner, g_outer);
}

Mode mode() { return g_active ? g_mode : Mode::Off; }

bool patchDeviceCreate(VkInstance inst, PFN_vkGetInstanceProcAddr gipa, VkPhysicalDevice pd, VkDeviceCreateInfo& ci, void* storage,
                       size_t storageSize) {
    if (g_mode == Mode::Off || storageSize < sizeof(VkPhysicalDeviceFragmentShadingRateFeaturesKHR)) return false;
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

void record(VkCommandBuffer cb) {
    if (!g_active || g_mode != Mode::Dynamic || !g_eye.image) return;
    // Gaze in VIEW space -> centre of the shared scene target (both eyes render into the same target size;
    // the average of the two eyes' gaze points keeps both foveae inside the full-rate region).
    XrPosef gz;
    if (!xr::input().gaze(gz)) return;
    const auto& v = xr::runtime().views();
    if (!v.valid) return;
    // gaze direction: -Z of the gaze orientation
    const auto& q = gz.orientation;
    float dx = -(2 * (q.x * q.z + q.w * q.y)), dy = -(2 * (q.y * q.z - q.w * q.x)), dz = -(1 - 2 * (q.x * q.x + q.y * q.y));
    if (dz > -0.1f) return;
    float tx = dx / -dz, ty = dy / -dz, cx = 0, cy = 0;
    for (int e = 0; e < 2; ++e) {
        const auto& f = v.fov[e];
        float l = std::tan(f.angleLeft), r = std::tan(f.angleRight), u = std::tan(f.angleUp), d = std::tan(f.angleDown);
        cx += (tx - l) / (r - l) * 0.5f;
        cy += (u - ty) / (u - d) * 0.5f;
    }
    cx = std::clamp(cx, 0.0f, 1.0f), cy = std::clamp(cy, 0.0f, 1.0f);
    if (std::fabs(cx - g_lastCx) < 0.01f && std::fabs(cy - g_lastCy) < 0.01f) return;
    g_lastCx = cx, g_lastCy = cy;
    g_ring = (g_ring + 1) % kRing;
    size_t off = size_t(kNeutralTexels) * kNeutralTexels + g_eyeBytes * g_ring;
    fillPattern(g_stagingPtr + off, g_eye.w, g_eye.h, cx, cy);
    copyIn(cb, g_eye, off, false);
}
}  // namespace fov
