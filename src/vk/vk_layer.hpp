#pragma once
// Vulkan interception: XR-mediated instance/device creation, queue serialisation, frame submission.
namespace vk_layer {
bool install();       // at payload attach: hooks GetProcAddress (game-module callers only)
void onGameVulkan();  // provided by the payload: first game Vulkan call (outside the loader lock)
}
