#pragma once
// Temporal upscaling / anti-aliasing for the VR eyes and the flat (desktop) main view: NVIDIA DLSS (DLSS-SR / DLAA,
// NGX Vulkan) and AMD FSR 3.1 (FidelityFX API, Vulkan). Tuned for VR:
//   - one context per view slot (each eye and the flat view keep their own history; the engine renders the eyes one
//     after the other);
//   - per-eye sub-pixel jitter from a Halton(2,3) sequence, the same index for both eyes in a frame;
//   - history reset on discontinuities (level load, teleport-like camera jumps, recentre);
//   - light sharpening (lenses magnify ringing), DLSS transformer preset K, DLAA at native resolution;
//   - a negative texture LOD bias of log2(render/display) so magnified textures stay sharp.
// Options (ss4smvr/options.txt):
//   upscaler        = auto | 0 off | 1 fsr | 2 dlss      (auto: DLSS on NVIDIA RTX when available, else FSR)
//   upscale_quality = 0 native AA (DLAA / FSR native) | 1 quality | 2 balanced | 3 performance   (default 0)
//   upscale_sharpness = 0..100 (default 25): the mod's RCAS sharpening after either backend; the game menu's
//                     "Upscaler Sharpening" slider replaces it (src/se4/gfx_menu.cpp)
//   upscale_phase_b = 1 render at output / ratio for quality 1..3 (default) | 0 render at output size (src/up/integrate.cpp)
//   desktop_upscaler = 1 desktop menu mode (VR off): DLSS and FSR 3.1 are both initialised and chosen at runtime from the
//                    game's Options > Graphics > Rescaling Filter list (src/se4/gfx_menu.cpp); the render size is the
//                    engine's own "Max 3D rendering Mpix" result unless upscale_quality is set (default 0)
#include <vulkan/vulkan.h>
#include <cstdint>

namespace up {
enum class Kind { Off, FSR, DLSS };

// Upscaler menu mode (docs/re/R28_rescaling_filter_menu.md, R29): desktop with option desktop_upscaler, or VR (default
// on; off with vr_upscaler_menu=0 or single_pass=1).
bool menuMode();
// An upscaler may run in this session: option upscaler set, or desktop menu mode.
bool wanted();
// The startup choice in desktop menu mode, in the encoding of option upscaler (-1 auto, 0 off, 1 FSR, 2 DLSS); set
// before init (src/se4/gfx_menu.cpp). Without it, option upscaler.
void setMenuStart(int opt);
// That startup choice (-2 when not set).
int menuStart();

// Extensions DLSS needs on the game's instance/device (added by the Vulkan layer at creation; in desktop menu mode
// always, when the driver's NGX core loads).
void instanceExtensions(const char** names, uint32_t& count, uint32_t max);
void deviceExtensions(VkInstance inst, VkPhysicalDevice pd, const char** names, uint32_t& count, uint32_t max);

// After device creation (the XR-redirected device), on the thread that created it.
bool init(VkInstance inst, VkPhysicalDevice pd, VkDevice dev, PFN_vkGetInstanceProcAddr gipa, PFN_vkGetDeviceProcAddr gdpa,
          VkQueue queue, uint32_t family, const wchar_t* modDirectory);
Kind active();
const char* name();
const char* name(Kind k);
// A backend that init brought up (desktop menu mode tries both; otherwise only the selected one). Off is always.
bool available(Kind k);
// Switch the active backend at runtime (desktop menu mode, src/up/integrate.cpp at a frame boundary): Off or an
// available backend. Every history is reset; the other backend keeps its contexts for a later switch back. False (no
// change) when `k` is not available.
bool select(Kind k);

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
// Upscale one view: slot 1 or 2 a VR eye, slot 0 the flat (desktop) main view (docs/re/R27_flat_upscaler.md); each
// slot keeps its own context and history. Returns false when no backend is active or the call failed (caller keeps
// its path).
bool evaluate(int slot, const Frame& f);
// Sharpening strength 0..1 of the mod's RCAS pass on the upscaled image (src/up/integrate.cpp), for both backends
// (FSR's own sharpener stays off; DLSS has none since 2.2.9). setSharpness: the menu, at any time.
float sharpness();
void setSharpness(float s);
// Drop all histories (level load, recentre); the next evaluate per slot starts fresh.
void resetHistory();
}  // namespace up
