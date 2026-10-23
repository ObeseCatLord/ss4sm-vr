#pragma once
// Vulkan interception: XR-mediated instance/device creation, queue serialisation, frame submission.
namespace vk_layer {
void onVulkanLoaded(void* vulkanModule);
}
