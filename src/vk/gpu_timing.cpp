#include "vk/gpu_timing.hpp"
#include "vk/queue_lock.hpp"
#include "se4/log.hpp"
#include "se4/options.hpp"
#include <windows.h>
#include <algorithm>
#include <vector>

namespace gputime {
namespace {
constexpr uint32_t kPairs = 256;  // submissions in flight at most (a frame makes a handful)
struct State {
    bool on = false;
    VkDevice dev = VK_NULL_HANDLE;
    VkQueryPool pool = VK_NULL_HANDLE;
    VkCommandBuffer begin[kPairs]{}, end[kPairs]{};
    bool pending[kPairs]{};
    uint32_t next = 0;
    double period = 1.0;    // ns per tick
    uint64_t mask = ~0ull;  // valid timestamp bits
    PFN_vkGetQueryPoolResults results = nullptr;
    // window sums
    double gpuNs = 0;
    uint32_t submits = 0;
    double cpuMs = 0, cpuMax = 0;
    uint32_t cpuFrames = 0;
    LARGE_INTEGER exitAt{}, freq{};
} g;

// Adds a finished pair's duration to the window (false while it is still in flight).
bool harvest(uint32_t i) {
    uint64_t r[4] = {};  // {begin, avail, end, avail}
    if (g.results(g.dev, g.pool, 2 * i, 2, sizeof r, r, 2 * sizeof(uint64_t), VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WITH_AVAILABILITY_BIT) !=
            VK_SUCCESS ||
        !r[1] || !r[3])
        return false;
    const uint64_t b = r[0] & g.mask, e = r[2] & g.mask;
    if (e >= b) g.gpuNs += double(e - b) * g.period, ++g.submits;
    return true;
}
// The next pair to wrap a submission with, its previous use harvested (a pair still in flight is skipped).
int take() {
    for (uint32_t k = 0; k < kPairs; ++k) {
        const uint32_t i = (g.next + k) % kPairs;
        if (g.pending[i] && !harvest(i)) continue;
        g.pending[i] = false;
        g.next = (i + 1) % kPairs;
        return int(i);
    }
    return -1;
}
bool wrappable(const void* pNext) {
    for (auto* h = static_cast<const VkBaseInStructure*>(pNext); h; h = h->pNext)
        if (h->sType == VK_STRUCTURE_TYPE_DEVICE_GROUP_SUBMIT_INFO || h->sType == VK_STRUCTURE_TYPE_PROTECTED_SUBMIT_INFO) return false;
    return true;
}
}  // namespace

void onDevice(VkInstance inst, PFN_vkGetInstanceProcAddr gipa, VkPhysicalDevice pd, VkDevice dev, PFN_vkGetDeviceProcAddr gdpa,
              uint32_t family) {
    if (!se4::options::get("perf_timing", 1)) return;
    auto props = reinterpret_cast<PFN_vkGetPhysicalDeviceProperties>(gipa(inst, "vkGetPhysicalDeviceProperties"));
    auto qprops = reinterpret_cast<PFN_vkGetPhysicalDeviceQueueFamilyProperties>(gipa(inst, "vkGetPhysicalDeviceQueueFamilyProperties"));
    if (!props || !qprops) return;
    VkPhysicalDeviceProperties p{};
    props(pd, &p);
    uint32_t n = 0;
    qprops(pd, &n, nullptr);
    std::vector<VkQueueFamilyProperties> q(n);
    qprops(pd, &n, q.data());
    if (family >= n || !q[family].timestampValidBits || !(p.limits.timestampPeriod > 0)) {
        VRLOG("perf: no timestamps on the XR queue - GPU timing off");
        return;
    }
    g.period = p.limits.timestampPeriod;
    g.mask = q[family].timestampValidBits >= 64 ? ~0ull : ((1ull << q[family].timestampValidBits) - 1);
#define DF(name) reinterpret_cast<PFN_##name>(gdpa(dev, #name))
    g.results = DF(vkGetQueryPoolResults);
    auto createPool = DF(vkCreateQueryPool);
    auto createCmdPool = DF(vkCreateCommandPool);
    auto alloc = DF(vkAllocateCommandBuffers);
    auto beginCB = DF(vkBeginCommandBuffer);
    auto endCB = DF(vkEndCommandBuffer);
    auto reset = DF(vkCmdResetQueryPool);
    auto stamp = DF(vkCmdWriteTimestamp);
#undef DF
    if (!g.results || !createPool || !createCmdPool || !alloc || !beginCB || !endCB || !reset || !stamp) return;
    VkQueryPoolCreateInfo qi{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
    qi.queryType = VK_QUERY_TYPE_TIMESTAMP;
    qi.queryCount = 2 * kPairs;
    VkCommandPoolCreateInfo ci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    ci.queueFamilyIndex = family;
    VkCommandPool cmdPool = VK_NULL_HANDLE;
    if (createPool(dev, &qi, nullptr, &g.pool) != VK_SUCCESS || createCmdPool(dev, &ci, nullptr, &cmdPool) != VK_SUCCESS) return;
    VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    ai.commandPool = cmdPool;
    ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    ai.commandBufferCount = kPairs;
    if (alloc(dev, &ai, g.begin) != VK_SUCCESS || alloc(dev, &ai, g.end) != VK_SUCCESS) return;
    // Recorded once: each pair's begin resets its two queries and stamps the top of the pipe, its end stamps the bottom.
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    for (uint32_t i = 0; i < kPairs; ++i) {
        beginCB(g.begin[i], &bi);
        reset(g.begin[i], g.pool, 2 * i, 2);
        stamp(g.begin[i], VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, g.pool, 2 * i);
        endCB(g.begin[i]);
        beginCB(g.end[i], &bi);
        stamp(g.end[i], VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, g.pool, 2 * i + 1);
        endCB(g.end[i]);
    }
    g.dev = dev;
    QueryPerformanceFrequency(&g.freq);
    g.on = true;
    VRLOG("perf: GPU timing on the XR queue (%.2f ns per tick, %u valid bits)", g.period, q[family].timestampValidBits);
}

VkResult submit(PFN_vkQueueSubmit real, VkQueue q, uint32_t n, const VkSubmitInfo* s, VkFence f) {
    if (!g.on || !n || !s) return real(q, n, s, f);
    for (uint32_t i = 0; i < n; ++i)
        if (!wrappable(s[i].pNext)) return real(q, n, s, f);
    const int i = take();
    if (i < 0) return real(q, n, s, f);
    // Reused buffers (submits are serialised by the queue lock): no heap allocation per submit once they have grown.
    static std::vector<VkSubmitInfo> infos;
    static std::vector<VkCommandBuffer> first, last;
    infos.assign(s, s + n), first.clear(), last.clear();
    first.push_back(g.begin[i]);
    first.insert(first.end(), s[0].pCommandBuffers, s[0].pCommandBuffers + s[0].commandBufferCount);
    if (n == 1) {
        first.push_back(g.end[i]);
    } else {
        last.assign(s[n - 1].pCommandBuffers, s[n - 1].pCommandBuffers + s[n - 1].commandBufferCount);
        last.push_back(g.end[i]);
        infos[n - 1].commandBufferCount = uint32_t(last.size());
        infos[n - 1].pCommandBuffers = last.data();
    }
    infos[0].commandBufferCount = uint32_t(first.size());
    infos[0].pCommandBuffers = first.data();
    const VkResult r = real(q, n, infos.data(), f);
    if (r == VK_SUCCESS) g.pending[i] = true;
    return r;
}

VkResult submit2(PFN_vkQueueSubmit2 real, VkQueue q, uint32_t n, const VkSubmitInfo2* s, VkFence f) {
    if (!g.on || !n || !s) return real(q, n, s, f);
    for (uint32_t i = 0; i < n; ++i)
        if (!wrappable(s[i].pNext) || (s[i].flags & VK_SUBMIT_PROTECTED_BIT)) return real(q, n, s, f);
    const int i = take();
    if (i < 0) return real(q, n, s, f);
    auto cbi = [](VkCommandBuffer cb) {
        VkCommandBufferSubmitInfo c{VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO};
        c.commandBuffer = cb;
        return c;
    };
    static std::vector<VkSubmitInfo2> infos;
    static std::vector<VkCommandBufferSubmitInfo> first, last;
    infos.assign(s, s + n), first.clear(), last.clear();
    first.push_back(cbi(g.begin[i]));
    first.insert(first.end(), s[0].pCommandBufferInfos, s[0].pCommandBufferInfos + s[0].commandBufferInfoCount);
    if (n == 1) {
        first.push_back(cbi(g.end[i]));
    } else {
        last.assign(s[n - 1].pCommandBufferInfos, s[n - 1].pCommandBufferInfos + s[n - 1].commandBufferInfoCount);
        last.push_back(cbi(g.end[i]));
        infos[n - 1].commandBufferInfoCount = uint32_t(last.size());
        infos[n - 1].pCommandBufferInfos = last.data();
    }
    infos[0].commandBufferInfoCount = uint32_t(first.size());
    infos[0].pCommandBufferInfos = first.data();
    const VkResult r = real(q, n, infos.data(), f);
    if (r == VK_SUCCESS) g.pending[i] = true;
    return r;
}

void presentEntry() {
    if (!g.on || !g.exitAt.QuadPart) return;
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    const double ms = double(now.QuadPart - g.exitAt.QuadPart) * 1000.0 / double(g.freq.QuadPart);
    if (ms > 0 && ms < 1000) g.cpuMs += ms, g.cpuMax = std::max(g.cpuMax, ms), ++g.cpuFrames;
}
void presentExit() {
    if (g.on) QueryPerformanceCounter(&g.exitAt);
}

void log(unsigned frames) {
    if (!g.on || !frames) return;
    vkq::Guard lock;  // the submit path updates these under the queue lock
    // Pairs finished but not yet reused are harvested now so the window is complete up to a frame or two.
    for (uint32_t i = 0; i < kPairs; ++i)
        if (g.pending[i] && harvest(i)) g.pending[i] = false;
    VRLOG("perf: GPU %.2f ms per frame (%.1f submissions), game CPU %.2f ms per frame (max %.2f), %u frames", g.gpuNs / 1e6 / frames,
          double(g.submits) / frames, g.cpuFrames ? g.cpuMs / g.cpuFrames : 0.0, g.cpuMax, frames);
    g.gpuNs = 0, g.submits = 0, g.cpuMs = 0, g.cpuMax = 0, g.cpuFrames = 0;
}
}  // namespace gputime
