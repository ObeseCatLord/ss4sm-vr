#pragma once
// Upscaler integration into the engine's eye render (docs/re/R13_mp_channel_upscaler.md s4, s5; lab 2026-10-24).
// Phase A: DLAA / FSR 3.1 native AA in place on the HDR canvas, per eye, right before post-processing.
//   - The engine allocates the HDR canvas (RGBA16F) and the scene depth (D32, reverse-Z) in 0x140c88a10; the mod
//     captures both VkImages there and adds SAMPLED usage to the depth.
//   - At the post-processing dispatch 0x140c89010 (the call whose PP reads the canvas; the other per-eye call
//     with flags 0 records nothing) the canvas is in COLOR_ATTACHMENT_OPTIMAL and the depth in
//     DEPTH_STENCIL_ATTACHMENT_OPTIMAL. The mod records into the engine's current command buffer: camera motion
//     vectors from depth (src/up/shaders/motion.comp), the upscaler, a copy back into the canvas, and restores both
//     layouts so the engine's own barriers stay valid.
//   - Projection jitter is applied in device slot 0x070 (src/se4/vr_device.cpp) through jitterProjection().
// Phase B (docs/re/R25_upscale_render_size.md; upscale_quality 1..3, option upscale_phase_b): the eye scene renders at
// out/ratio and post-processing runs at the eye size.
//   - The MPix limiter's RenderView call returns round(out/ratio) for eyes 1/2; its HDR-allocation call returns its
//     input, and device slot 0x068 reports the eye size, so the HDR canvas stays at the eye size. No cvar is written.
//   - The upscaler fills the canvas at the eye size (a bilinear blit if it fails), post-processing reads that box and
//     writes the eye drawport, and the engine's rescale of its 3D canvas into the eye drawport is skipped.
//   - Any mismatch takes the stock path: the engine rescales the render-size image spatially.
#include "core/stereo.hpp"
#include <vulkan/vulkan.h>
#include <cstdint>
#include <vector>

namespace upint {
bool install();
// Desktop play (VR not enabled) with an upscaler selected (docs/re/R27_flat_upscaler.md): the flat main view is
// upscaled as slot 0. RenderView of the main view (inside the player-views pass, current drawport on the main
// drawport's texture) is classified at entry; its MPix limiter call returns the identity (phase A) or out/ratio
// (phase B), reads the camera (gfx+0xc70 doubles, P at gfx+0xc2c) and jitters gfx+0xc2c, which end_view restores;
// its PP (ppcmd_pp_call + 5, dst = view + 0x288) evaluates the HDR box. Off with MSAA; every flat branch requires no
// VR device.
bool flatWanted();
// Phase B is on now (two-pass stereo, an upscaler running at a ratio > 1, symbols and opcodes verified).
bool phaseB();
// Vulkan layer callbacks.
void onDevice(VkInstance inst, PFN_vkGetInstanceProcAddr gipa, VkPhysicalDevice pd, VkDevice dev, PFN_vkGetDeviceProcAddr gdpa);
// Before vkCreateImage: the HDR allocation's depth gets SAMPLED usage while an upscaler reads it.
void patchImage(VkImageCreateInfo& ci);
void onImageCreated(VkImage image, const VkImageCreateInfo& ci);
void onBeginCommandBuffer(VkCommandBuffer cb);
// Scene passes (RGBA16F colour + depth): keep the depth contents (storeOp STORE) while an upscaler reads it.
// `att` receives the patched attachment array. True when patched.
bool patchPass(VkRenderPassCreateInfo& ci, std::vector<VkAttachmentDescription>& att);
bool patchPass2(VkRenderPassCreateInfo2& ci, std::vector<VkAttachmentDescription2>& att);
void onBeginPass(VkCommandBuffer cb);
void onEndPass(VkCommandBuffer cb);
void onImageBarrier(VkCommandBuffer cb, VkImage image, VkImageLayout from, VkImageLayout to);

// Device slot 0x070 for eye 1/2: records the unjittered frustum (motion vectors) and returns it jittered for this
// frame while an upscaler runs. `frameTime` is the XR predicted display time (advances the jitter sequence).
core::Fov jitterProjection(int eye, const core::Fov& fov, float nearZ, float farZ, int64_t frameTime, void* caller = nullptr);
}  // namespace upint
