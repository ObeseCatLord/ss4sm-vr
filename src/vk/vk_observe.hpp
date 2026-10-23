#pragma once
// Read-only observation of the game's Vulkan setup (Phase 0/1 evidence): hooks vulkan-1.dll's
// vkGetInstanceProcAddr / vkCreateInstance / vkCreateDevice and logs what the engine requests.
namespace vk_observe {
void onVulkanLoaded(void* vulkanModule);
}
