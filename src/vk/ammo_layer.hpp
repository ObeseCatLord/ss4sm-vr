#pragma once
// The on-weapon ammo counters (src/se4/ammo_counter.hpp) as OpenXR quad layers: one swapchain, the left hand's readout in
// the top half and the right hand's in the bottom half, drawn on the CPU (src/core/ammo_counter.hpp) into a
// host-visible staging ring and copied in.
#include <openxr/openxr.h>
#include <vulkan/vulkan.h>

namespace ammolayer {
void onDevice(VkInstance inst, PFN_vkGetInstanceProcAddr gipa, VkPhysicalDevice pd, VkDevice dev, PFN_vkGetDeviceProcAddr gdpa);
// Present thread, before recording: which hands show a counter; acquires the swapchain image. 0: no layer this frame.
int begin();
// Records the upload into the present command buffer (outside any render pass). `slot`: the caller's frame-in-flight ring
// index (0..2), whose fence was waited on.
void record(VkCommandBuffer cb, int slot);
void end();
// Fills up to two quad layers (play space) for the hands begin() reported; returns how many.
int layers(XrSpace space, XrCompositionLayerQuad out[2]);
}  // namespace ammolayer
