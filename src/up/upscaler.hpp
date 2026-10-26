#pragma once
// Temporal upscaling / anti-aliasing for the VR eyes: NVIDIA DLSS (DLSS-SR / DLAA, NGX Vulkan) and AMD FSR 3.1
// (FidelityFX API, Vulkan). Tuned for VR:
//   - one context per eye (each eye keeps its own history; the engine renders the eyes one after the other);
//   - per-eye sub-pixel jitter from a Halton(2,3) sequence, the same index for both eyes in a frame;
//   - history reset on discontinuities (level load, teleport-like camera jumps, recentre);
//   - light sharpening (lenses magnify ringing), DLSS transformer preset K, DLAA at native resolution;
//   - a negative texture LOD bias of log2(render/display) so magnified textures stay sharp.
// Options (ss4smvr/options.txt):
//   upscaler        = auto | 0 off | 1 fsr | 2 dlss      (auto: DLSS on NVIDIA RTX when available, else FSR)
//   upscale_quality = 0 native AA (DLAA / FSR native) | 1 quality | 2 balanced | 3 performance   (default 0)
//   upscale_sharpness = 0..100 (default 15)
//   upscale_phase_b = 1 render at output / ratio for quality 1..3 (default) | 0 render at output size (src/up/integrate.cpp)
#include <vulkan/vulkan.h>
#include <cstdint>

namespace up {
enum class Kind { Off, FSR, DLSS };

// Extensions DLSS needs on the game's instance/device (added by the Vulkan layer at creation).
void instanceExtensions(const char** names, uint32_t& count, uint32_t max);
void deviceExtensions(VkInstance inst, VkPhysicalDevice pd, const char** names, uint32_t& count, uint32_t max);

// After device creation (the XR-redirected device), on the thread that created it.
bool init(VkInstance inst, VkPhysicalDevice pd, VkDevice dev, PFN_vkGetInstanceProcAddr gipa, PFN_vkGetDeviceProcAddr gdpa,
          VkQueue queue, uint32_t family, const wchar_t* modDirectory);
Kind active();
const char* name();

// Output / render ratio of the configured quality mode (1 for native AA and while no backend is active).
float ratio();
// Render resolution for an output (display) size under the configured quality mode.
void renderSize(uint32_t outW, uint32_t outH, uint32_t& renderW, uint32_t& renderH);
// Sub-pixel jitter for this frame in render pixels (x right, y down), and as a clip-space offset for the
// projection matrix (2*jitter/renderSize, y flipped for clip space).
void jitter(uint32_t frameIndex, uint32_t renderW, uint32_t outW, float& px, float& py);
// Texture LOD bias for samplers while upscaling.
float mipBias(uint32_t renderW, uint32_t outW);

struct Image {
    VkImage image = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    VkFormat format = VK_FORMAT_UNDEFINED;
    uint32_t width = 0, height = 0;
};
struct Frame {
    VkCommandBuffer cmd = VK_NULL_HANDLE;  // recording, outside a render pass; images in GENERAL or read layouts
    Image color, depth, motion, output;    // color/depth/motion hold the render size, output at display size (storage)
    uint32_t renderW = 0, renderH = 0;     // render size: sub-rect at (0,0) of color/depth/motion; 0 = their full extent
    float jitterX = 0, jitterY = 0;        // render pixels, as passed to jitter()
    float motionScaleX = 1, motionScaleY = 1;  // converts the engine's motion vectors to render pixels
    float frameMs = 11.1f;
    float nearZ = 0.1f, farZ = 1000.0f, fovY = 1.6f;
    bool depthInverted = false, hdr = true, reset = false;
};
// Upscale one eye (1 or 2). Returns false when no backend is active or the call failed (caller keeps its path).
bool evaluate(int eye, const Frame& f);
// Drop all histories (level load, recentre); the next evaluate per eye starts fresh.
void resetHistory();
}  // namespace up
