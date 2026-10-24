#pragma once
// Vulkan interception: XR-mediated instance/device creation, queue serialisation, frame submission.
// Entry points are substituted through se4/procaddr (vk_layer::interceptProc).
namespace vk_layer {
void onGameVulkan();  // provided by the payload: first game Vulkan call (outside the loader lock)
}
