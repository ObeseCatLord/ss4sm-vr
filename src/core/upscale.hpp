#pragma once
// Render size, jitter sequence and texture mip bias of the temporal upscalers (src/up/upscaler.cpp,
// docs/re/R25_upscale_render_size.md s4). Engine-agnostic; tests/upscale_checks.cpp.
#include <algorithm>
#include <cmath>
#include <cstdint>

namespace core {
// Output / render ratio per upscale_quality: 0 native AA, 1 quality, 2 balanced, 3 performance.
inline constexpr float kUpscaleRatio[4] = {1.0f, 1.5f, 1.7f, 2.0f};
inline float upscaleRatio(int quality) { return kUpscaleRatio[std::clamp(quality, 0, 3)]; }

// Render size for an output size: round(out / ratio), at least 1 pixel.
inline void upscaleRenderSize(uint32_t outW, uint32_t outH, float ratio, uint32_t& rw, uint32_t& rh) {
    if (!(ratio >= 1.0f)) ratio = 1.0f;
    rw = std::max(1u, uint32_t(std::lround(float(outW) / ratio)));
    rh = std::max(1u, uint32_t(std::lround(float(outH) / ratio)));
}

// Halton phase count: 8 * (output/render)^2, at least 8 (FSR's rule, also fine for DLSS). The render size is rounded,
// so the ratio is snapped to hundredths first: 1.5, 1.7 and 2.0 give 18, 24 and 32 whatever the rounding.
inline int jitterPhases(uint32_t renderW, uint32_t outW) {
    if (!renderW || !outW) return 8;
    const float r = std::round(100.0f * float(outW) / float(renderW)) / 100.0f;
    return std::max(8, int(std::ceil(8.0f * r * r - 1e-3f)));
}

inline float halton(int index, int base) {
    float f = 1, r = 0;
    for (int i = index; i > 0; i /= base) {
        f /= float(base);
        r += f * float(i % base);
    }
    return r;
}

// Sub-pixel jitter of a frame in render pixels, in (-0.5, 0.5): Halton(2,3) over jitterPhases.
inline void jitterOffset(uint32_t frame, uint32_t renderW, uint32_t outW, float& px, float& py) {
    const int idx = int(frame % uint32_t(jitterPhases(renderW, outW))) + 1;
    px = halton(idx, 2) - 0.5f;
    py = halton(idx, 3) - 0.5f;
}

// Texture LOD bias for a render width shown at an output width: log2(render/output).
inline float upscaleMipBias(uint32_t renderW, uint32_t outW) {
    return renderW && outW ? std::log2(float(renderW) / float(outW)) : 0.0f;
}
}  // namespace core
