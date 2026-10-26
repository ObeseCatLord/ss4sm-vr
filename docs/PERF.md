# Performance matrix

How it is measured:
- The tool is `tools/lab/perf_matrix.sh`.
- The mod's frame timing is `src/vk/gpu_timing.cpp`, option `perf_timing`, default on:
  - **GPU ms** is the sum of the timestamped durations of every submission on the XR queue in a frame. GPU idle gaps
    between submissions are left out.
  - **CPU ms** is the game's own time per frame, from the present's exit to the next present's entry, without the XR
    frame wait.
  - Values are medians of the 5 s windows in the minute after a 40 s settle. **CPU max** is the worst window's
    average.
- The lab compositor paces at 60 Hz, so frame rate is not the measure. On the headset, a frame fits 90 Hz when the
  GPU time and the CPU time each stay under 11.1 ms.

## Lab, 2026-10-26: SS4 Pompeii start, still head, RTX 4090, Proton GE 11-9, Vulkan, simulated Monado
| options | GPU ms | CPU ms | CPU max | eye size |
|---|---|---|---|---|
| render_scale=100 (lab default) | 3.03 | 6.02 | 10.57 | 1344x1512 |
| render_scale=136 | 4.60 | 5.94 | 11.56 | 1828x2056 |
| render_scale=170 | 4.66 | 6.06 | 7.97 | 1920x2160 (lab maximum) |
| render_scale=204 | 4.71 | 6.08 | 9.58 | 1920x2160 (clamped) |
| 170, single_pass=1 | 4.76 | **5.14** | 8.61 | 1920x2160 |
| 170, foveation=1 (fixed) | 4.72 | 6.06 | 10.47 | 1920x2160 |
| 170, upscaler=2 (DLSS/DLAA, preset K) | 5.37 | 6.31 | 21.63 | 1920x2160 |
| 170, upscaler=2, upscale_quality=1 | 5.36 | 6.39 | 16.45 | 1920x2160 (renders at full size, see below) |
| 170, upscaler=1, upscale_quality=1 (FSR 3.1) | 5.22 | 6.40 | 19.98 | 1920x2160 (renders at full size) |

Findings:
- **Resolution.** The lab runtime caps the eye size at 1920x2160 (4.1 MP per eye). A Beyond eye is about
  2560x2560 (6.5 MP). Scaling the measured 4.7 ms by pixel count gives roughly 7–8 ms on the headset, inside
  11.1 ms. Measure it there (USER_VR_TEST step 9).
- **Two-pass vs single pass.** Single pass no longer crashes; the cause was the released-target bug. It saves about
  15 % of the game's CPU time at the same GPU time.
  - Its flat HUD was missing at first: the HUD renders with the device's eye field at 0. That is fixed
    (`vrdev::hudEye`).
  - With `SUITE_OPTS=single_pass=1`, the core and vehicle stages pass in SS4 (17/17) and SM (17/17).
  - **Single pass is the default** (`single_pass=0` for two-pass). Selecting an upscaler defaults to two-pass, because
    the upscalers are integrated there.
- **Fixed foveation** gives no measurable GPU saving in this scene at this size, where the 4090 is not shading-bound.
  Check it again at headset resolution and in a dense scene.
- **Upscalers.**
  - Native-AA mode (DLAA, or FSR at ratio 1) costs about 0.6–0.7 ms per frame.
  - **Phase B (R25, 2026-10-26):** with `upscale_quality` 1–3 the scene now renders at output/ratio. The
    RenderView limiter is hooked for eyes 1/2, the upscaler fills the full-size HDR canvas, post-processing writes the
    eye image at full size, and the engine's spatial rescale is skipped.
    - Lab check, DLSS at ratio 1.5 (1280x1440 → 1920x2160): the limiter, the HDR canvas created once at eye size,
      projection, rescale skips and phase-B eye dispatches all agree.
    - About 98.6 % of eye frames take phase B. The rest fall back to a second stock post-processing pass.
    - GPU **5.12 ms**, against 5.37 ms for DLAA and 4.66 ms without AA. This scene is not shading-bound on the 4090,
      so the saving will be larger at headset resolution and in dense scenes.
    - Phase B needs two-pass. It is off with `upscale_phase_b=0`.
  - Image quality is in IMPLEMENTATION_STATUS (upscaler item). The default stays off.
- **CPU.** About 6 ms per frame in Pompeii, with peaks of 8–12 ms during loading or streaming windows. That is
  inside 11.1 ms, but busy scenes should be checked on the headset.

Still to measure:
- the flat baselines (Vulkan vs D3D11 vs D3D12, stock game);
- a dense combat scene with a fixed view (the Survival test pushed the camera into walls, so its GPU numbers depended
  on chance);
- SM;
- the headset (Beyond resolution, 90 Hz).
