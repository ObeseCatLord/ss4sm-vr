#pragma once
// Foveated rendering through a Vulkan fragment-shading-rate attachment on the engine's 3D scene passes
// (docs/research/DLSS_FOVEATION.md B6). It works on every runtime and headset, including streamed ones such as
// the Steam Frame, whose foveated streaming happens in the encoder and needs nothing from the application.
//   Dynamic: the coarse periphery follows the eye gaze (XR_EXT_eye_gaze_interaction). Default on a Steam Frame.
//   Fixed:   the full-rate region is around the lens centres. Option (foveation=fixed).
//   Off:     default on every other headset.
// Option foveation=auto|off|fixed|dynamic (auto = dynamic on a Steam Frame with eye gaze, else off), plus
// foveation_inner / foveation_outer (percent of the image height) for the full-rate and half-rate radii.
#include <vulkan/vulkan.h>
#include <cstdint>

namespace fov {
enum class Mode { Off, Fixed, Dynamic };
// After the XR runtime is up and before the game's device is created.
void decide();
Mode mode();
// Device creation (redirected through the XR runtime): enable attachment shading rates on the game's chain.
// `storage` must outlive the create call. Returns false (and changes nothing) when unsupported.
bool patchDeviceCreate(VkInstance inst, PFN_vkGetInstanceProcAddr gipa, VkPhysicalDevice pd, VkDeviceCreateInfo& ci, void* storage,
                       size_t storageSize);
void onDevice(VkInstance inst, PFN_vkGetInstanceProcAddr gipa, PFN_vkGetDeviceProcAddr gdpa, VkPhysicalDevice pd, VkDevice d,
              VkQueue queue, uint32_t family);
// Device-level entry points the module replaces (render passes, framebuffers, pipelines, shading-rate state).
PFN_vkVoidFunction override(const char* name);
// The eye render size (scene targets of this size get the foveation pattern).
void setEyeSize(uint32_t w, uint32_t h);
// Once per presented frame, inside the present hook's command buffer: upload a changed pattern.
void record(VkCommandBuffer cb);
}  // namespace fov
