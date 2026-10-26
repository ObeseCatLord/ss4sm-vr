#pragma once
// Frame timing for the performance matrix (docs/PERF.md): GPU time per frame from timestamps around every submission
// on the XR queue (a begin command buffer prepended, an end one appended; their sum over a frame leaves out the GPU
// idle gaps between submissions), and the game's CPU time per frame (present exit to the next present entry, i.e.
// without the XR frame wait). Logged with the mirror's 5 s statistics. Option perf_timing (default 1).
#include <vulkan/vulkan.h>

namespace gputime {
void onDevice(VkInstance inst, PFN_vkGetInstanceProcAddr gipa, VkPhysicalDevice pd, VkDevice dev, PFN_vkGetDeviceProcAddr gdpa,
              uint32_t family);
// Called under the queue lock in place of the real submit.
VkResult submit(PFN_vkQueueSubmit real, VkQueue q, uint32_t n, const VkSubmitInfo* s, VkFence f);
VkResult submit2(PFN_vkQueueSubmit2 real, VkQueue q, uint32_t n, const VkSubmitInfo2* s, VkFence f);
// Present thread: entry and exit of the game's present (the XR frame wait happens inside).
void presentEntry();
void presentExit();
// Logs the averages since the last call over `frames` frames.
void log(unsigned frames);
}  // namespace gputime
