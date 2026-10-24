#pragma once
// The scope view (engine eye 3, src/se4/scope.cpp) as an OpenXR quad layer: copied out of the engine image, masked to
// a circle with a reticle (src/up/shaders/scope.comp), and copied into the layer's swapchain.
#include "vk/eye_images.hpp"
#include <openxr/openxr.h>
#include <vulkan/vulkan.h>

namespace scopelayer {
void onDevice(VkInstance inst, PFN_vkGetInstanceProcAddr gipa, VkPhysicalDevice pd, VkDevice dev, PFN_vkGetDeviceProcAddr gdpa);
// Present thread, before recording: swapchain (re)creation and image acquire. False: no layer this frame.
bool begin(const eyes::ScopeFrame& f);
// Records into the present command buffer (outside any render pass). The engine image is in TRANSFER_SRC_OPTIMAL.
void record(VkCommandBuffer cb, const eyes::ScopeFrame& f, bool reticle);
// After the submit: release the swapchain image.
void end();
XrSwapchain swapchain();
uint32_t size();
}  // namespace scopelayer
