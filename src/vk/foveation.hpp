#pragma once
// Foveated rendering through a Vulkan fragment-shading-rate attachment on the engine's 3D scene passes
// (docs/research/DLSS_FOVEATION.md B6). It works on every runtime and headset, including streamed ones such as
// the Steam Frame, whose foveated streaming happens in the encoder and needs nothing from the application.
//   Dynamic: the coarse periphery follows the eye gaze (XR_EXT_eye_gaze_interaction), each eye's pattern centred on its
//            own gaze point. Default on every headset whose runtime offers eye gaze (Steam Frame, Quest Pro, ...).
//   Fixed:   the full-rate region is around each lens centre. The option for headsets without eye tracking.
//   Off:     default without eye tracking.
// Quad views were weighed and not taken (docs/research/DLSS_FOVEATION.md option 2, R31): SteamVR (the Steam Frame's PC
// runtime) cannot consume them, and the engine would have to render the inset as extra views.
// Option foveation=-1 auto|0 off|1 fixed|2 eye-tracked at startup (the VR graphics menu row changes it at runtime),
// foveation_inner / foveation_fixed_inner (full-rate radius, percent of the image height; 2x2 beyond), and with
// foveation_coarse=4 a 4x4 ring beyond foveation_outer / foveation_fixed_outer. Details and lab numbers: R31.
#include <vulkan/vulkan.h>
#include <cstdint>

namespace fov {
enum class Mode { Off, Fixed, Dynamic };
// After the XR runtime is up and before the game's device is created.
void decide();
Mode mode();
// The device was patched (attachment shading rates available): the mode can change at runtime.
bool available();
// The runtime offers eye gaze (XR_EXT_eye_gaze_interaction).
bool gazeSupported();
// Runtime switch (the VR graphics menu); Dynamic falls back to Fixed without eye gaze. Any thread.
void setMode(Mode m);
// The VR menu's saved choice (-1 auto, 0 off, 1 fixed, 2 eye-tracked), before decide(); options.txt foveation wins.
void setStartChoice(int m);
// When eye 1/2's view starts recording: that eye's pattern (gaze or lens centre), with the rendered image covering the
// top-left sx x sy of the target (upscaler phase B). The upload is recorded before the eye's first scene pass (the
// engine's command buffer is inside a render pass when the view starts).
void beforeEye(int eye, float sx, float sy);
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
