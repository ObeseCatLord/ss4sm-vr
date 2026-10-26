// Upscaler render size, jitter phases and mip bias (core/upscale.hpp; docs/re/R25_upscale_render_size.md s0.4, s0.5).
#undef NDEBUG
#include "core/upscale.hpp"
#include <cassert>
#include <cmath>
#include <cstdio>
#include <set>
#include <utility>

using namespace core;

int main() {
    // Quality modes and their ratios; out-of-range options clamp like upscale_quality does.
    assert(upscaleRatio(0) == 1.0f && upscaleRatio(1) == 1.5f && upscaleRatio(2) == 1.7f && upscaleRatio(3) == 2.0f);
    assert(upscaleRatio(-1) == 1.0f && upscaleRatio(7) == 2.0f);

    // Render size = round(out / ratio).
    uint32_t rw, rh;
    upscaleRenderSize(1920, 2160, 1.0f, rw, rh);
    assert(rw == 1920 && rh == 2160);
    upscaleRenderSize(1920, 2160, 1.5f, rw, rh);
    assert(rw == 1280 && rh == 1440);
    upscaleRenderSize(1920, 2160, 1.7f, rw, rh);
    assert(rw == 1129 && rh == 1271);  // 1129.41, 1270.59
    upscaleRenderSize(1920, 2160, 2.0f, rw, rh);
    assert(rw == 960 && rh == 1080);
    upscaleRenderSize(2001, 1, 2.0f, rw, rh);
    assert(rw == 1001 && rh == 1);  // 1000.5 rounds up; never 0
    upscaleRenderSize(1344, 1512, 0.5f, rw, rh);
    assert(rw == 1344 && rh == 1512);  // a ratio below 1 is not an upscale
    // The aspect error from rounding stays under one render pixel.
    for (uint32_t w = 800; w < 3000; w += 37)
        for (float r : {1.5f, 1.7f, 2.0f}) {
            const uint32_t h = w * 9 / 8;
            upscaleRenderSize(w, h, r, rw, rh);
            assert(std::fabs(float(rw) * r - float(w)) <= 0.5f * r + 1e-3f);
            assert(std::fabs(float(rh) - float(rw) * float(h) / float(w)) < 1.0f);
        }

    // Phase count 8 * ratio^2: 8, 18, 24, 32, whatever the rounding of the render size.
    assert(jitterPhases(1920, 1920) == 8);
    assert(jitterPhases(0, 1920) == 8);
    const std::pair<float, int> expect[] = {{1.0f, 8}, {1.5f, 18}, {1.7f, 24}, {2.0f, 32}};
    for (auto [r, phases] : expect)
        for (uint32_t w = 1000; w < 3000; w += 1) {
            upscaleRenderSize(w, w, r, rw, rh);
            assert(jitterPhases(rw, w) == phases);
        }

    // Jitter: in (-0.5, 0.5), all phases distinct, the sequence repeats after the phase count.
    for (auto [r, phases] : expect) {
        upscaleRenderSize(1920, 2160, r, rw, rh);
        std::set<std::pair<float, float>> seen;
        float sx = 0, sy = 0;
        for (int i = 0; i < phases; ++i) {
            float px, py, qx, qy;
            jitterOffset(uint32_t(i), rw, 1920, px, py);
            jitterOffset(uint32_t(i + phases), rw, 1920, qx, qy);
            assert(px > -0.5f && px < 0.5f && py > -0.5f && py < 0.5f);
            assert(px == qx && py == qy);
            seen.insert({px, py});
            sx += px, sy += py;
        }
        assert(int(seen.size()) == phases);
        assert(std::fabs(sx / float(phases)) < 0.1f && std::fabs(sy / float(phases)) < 0.1f);  // centred
    }
    assert(halton(1, 2) == 0.5f && halton(2, 2) == 0.25f && std::fabs(halton(1, 3) - 1.0f / 3.0f) < 1e-6f);

    // Mip bias log2(render/output) (R23 + R25 s0.5): 0 at native AA, about -0.585, -0.766, -1 for the quality modes.
    assert(upscaleMipBias(1920, 1920) == 0.0f);
    assert(upscaleMipBias(0, 1920) == 0.0f && upscaleMipBias(1920, 0) == 0.0f);
    assert(std::fabs(upscaleMipBias(1280, 1920) + 0.585f) < 1e-3f);
    upscaleRenderSize(1920, 2160, 1.7f, rw, rh);
    assert(std::fabs(upscaleMipBias(rw, 1920) + 0.766f) < 2e-3f);
    assert(upscaleMipBias(960, 1920) == -1.0f);

    // Quality mode of an actual ratio (DLSS creation in desktop menu mode): the configured ratios map back to their mode.
    assert(upscaleQualityOfRatio(1920, 1920) == 0 && upscaleQualityOfRatio(0, 1920) == 0 && upscaleQualityOfRatio(1920, 0) == 0);
    assert(upscaleQualityOfRatio(2000, 1920) == 0);  // larger than the output: not an upscale
    for (int q = 0; q <= 3; ++q) {
        upscaleRenderSize(2560, 1440, upscaleRatio(q), rw, rh);
        assert(upscaleQualityOfRatio(rw, 2560) == q);
    }
    assert(upscaleQualityOfRatio(1932, 2560) == 1);  // R28 s4 example: 2.1 MPix at 1440p, ratio 1.325
    assert(upscaleQualityOfRatio(1800, 1920) == 1);  // any upscale is at least the quality mode
    assert(upscaleQualityOfRatio(1600, 2560) == 2);  // ratio 1.6: balanced
    assert(upscaleQualityOfRatio(1400, 2560) == 3);  // ratio 1.83: performance
    assert(upscaleQualityOfRatio(1200, 2560) == 4);  // ratio 2.13: ultra performance
    assert(upscaleQualityOfRatio(854, 2560) == 4);   // ratio 3: ultra performance

    // Desktop menu render size: the engine's limiter result clamped to [ceil(out/3), out] per axis.
    menuRenderSize(2560, 1440, 1932, 1087, rw, rh);  // R28 s4 example
    assert(rw == 1932 && rh == 1087);
    menuRenderSize(2560, 1440, 2560, 1440, rw, rh);  // cap Unlimited or >= 95 %: native AA
    assert(rw == 2560 && rh == 1440);
    menuRenderSize(2560, 1440, 4000, 3000, rw, rh);  // never above the output
    assert(rw == 2560 && rh == 1440);
    menuRenderSize(3840, 2160, 640, 360, rw, rh);  // 4K with a 0.23 MPix limit: clamped to a third
    assert(rw == 1280 && rh == 720);
    menuRenderSize(1000, 1001, 1, 1, rw, rh);  // ceil(out/3)
    assert(rw == 334 && rh == 334);
    menuRenderSize(1920, 1080, 0, -5, rw, rh);  // no engine size: the output size
    assert(rw == 1920 && rh == 1080);
    menuRenderSize(0, 1080, 100, 100, rw, rh);
    assert(rw == 0 && rh == 360);
    for (uint32_t w = 3; w < 4000; w += 13)
        for (int e : {1, int(w / 3), int(w / 2), int(w), int(w) + 7}) {
            menuRenderSize(w, w, e, e, rw, rh);
            assert(rw >= (w + 2) / 3 && rw <= w && rw * 3 >= w && rw == rh);
        }

    std::puts("upscale checks passed");
    return 0;
}
