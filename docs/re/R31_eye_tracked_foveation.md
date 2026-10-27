# R31: eye-tracked foveated rendering, swing-kill death animations (Sam4.exe 616154, SamSM.exe 628492)

Sources: `docs/research/DLSS_FOVEATION.md` (part B: Steam Frame, SteamVR, Monado, wineopenxr), the Steamworks Steam Frame
custom-engine page, lab runs on the isolated SS4 copy (lab-only option `lab_fov_gaze`).

## 1. Method: VRS shading-rate attachment, not quad views

- **Steam Frame (Steamworks):** eye tracking is `XR_EXT_eye_gaze_interaction`. The recommended `XR_FB_foveation*` /
  `XR_META_foveation_eye_tracked` exist only on the on-headset (standalone, Adreno) runtime. A PC game streamed to the
  Frame runs on PC SteamVR, which offers `XR_EXT_eye_gaze_interaction` and **no** quad views and no FB/META foveation.
  The FB path also needs `VK_EXT_fragment_density_map`, which the RTX 4090 driver does not expose.
- **Quad views** (`XR_VARJO_quad_views`, OpenXR 1.1 foveated inset): unavailable on SteamVR, so not on the Frame. On
  Monado they are emulated with a fixed half-FOV inset. For this engine they would mean rendering 4 views (the
  engine's stereo is 2 eyes: two-pass, or single-pass double-wide), 4 upscaler instances, and a second set of HDR,
  post-processing and HUD targets. The saving (a smaller context view plus a small inset) is real on a GPU-bound
  game, but it is unavailable on the target headset and its cost in engine work is far larger.
- **Chosen: `VK_KHR_fragment_shading_rate` attachment** (vk/foveation.cpp). It works on every PC runtime, needs no
  engine view changes, combines with DLSS / FSR (they see a normal image with softer edges) and follows the gaze each
  frame:
  - The device gets `attachmentFragmentShadingRate` (always when the GPU offers it, so the menu can switch at
    runtime; Off uploads a full-rate pattern). `foveation_device=0` leaves the device untouched.
  - Scene render passes (depth + colour) get a shading-rate attachment. Eye-size framebuffers get the eye image, the
    rest a neutral full-rate image.
  - Pattern: full rate inside `inner`, then 2x2 (radii in units of the image height, round on screen). Eye-tracked:
    full rate within 0.20, fixed: within 0.30 (options `foveation_inner`, `foveation_fixed_inner`, percent). With
    `foveation_coarse=4`, the area beyond `outer` (0.40 eye-tracked, 0.50 fixed; `foveation_outer`,
    `foveation_fixed_outer`) is 4x4, except below the output size (upscaler phase B) (§6).

## 2. Why the earlier fixed foveation did nothing

- The 2026-10-26 lab entry "fixed foveation gives no measurable saving" was not a scene limit: **no fragment was
  ever shaded coarsely.** With an extreme pattern (full rate only within 5 % of the image height, 4x4 beyond 10 %)
  the eye image was pixel-identical in sharpness and the GPU time unchanged.
- Cause: the engine creates its scene pipelines with `VK_DYNAMIC_STATE_FRAGMENT_SHADING_RATE_KHR` (for its own
  `ren_bSceneVRS` / `prt_bEnableVRS` per-draw rates) and records `vkCmdSetFragmentShadingRateKHR` only while those
  are on. For such a pipeline the pipeline-state combiner is ignored and the dynamic combiner ops were never set,
  so the attachment rate was dropped.
- Fix: the layer remembers pipelines with that dynamic state, and after each `vkCmdBindPipeline` of one records
  `vkCmdSetFragmentShadingRateKHR(engine's last size, {engine's op0, MAX})`. Every graphics pipeline also gets the
  MAX attachment combiner in its static state (the engine creates pipelines against compatible render passes that
  are not always the patched handles; a pass without the attachment reads it as 1x1).
- Check, lab (Pompeii intro, 1920x2160 per eye, two-pass, eye dumps via `lab_eye_dump`): with the extreme pattern
  the periphery is visibly 4x4 and the centre sharp; GPU 3.23 → 2.65 ms per frame.

## 3. Per-eye patterns

- **Two-pass stereo:** both eyes render into the same eye-size scene framebuffers. The execute-view hook
  (`execute_view`, also used by the upscaler) notes which pattern the eye needs; the engine's command buffer is inside
  a render pass at that point, so the upload (copy from a host-visible ring of 8 slots, barriers from/to
  `FRAGMENT_SHADING_RATE_ATTACHMENT_OPTIMAL`) is recorded in the begin-render-pass hook just before the eye's first
  pass on an eye framebuffer. It is recorded only when the image holds something else.
- **Centre:** the eye's own projection (engine tangents, `vrdev::eyeFov`, shared vertical):
  - fixed: the optical axis, `cx = -tan(l) / (tan(r) - tan(l))`, `cy = tan(u) / (tan(u) - tan(d))`, not the image
    centre (asymmetric frusta);
  - eye-tracked: the gaze direction in view space (`xr::input().gaze`, -Z of its orientation) intersected with each
    eye's tangent plane. Without a gaze this frame (blink, lost tracking) that eye uses the fixed centre and radii.
- **Upscaler phase B** renders into the top-left `rw/ow` of the eye targets: the pattern is laid over that sub-rect.
- **Single-pass stereo** renders both eyes into one double-wide target (`docs/re/EYE_TARGETS.md`). Its framebuffers
  get a double-wide rate image with eye 1's pattern in the left half and eye 2's in the right, updated once per
  present. (Eye-size framebuffers without per-eye views fall back to one pattern at the average centre.)

## 4. Defaults and the menu

- Automatic (no choice saved): **eye-tracked when the runtime reports `supportsEyeGazeInteraction`** (Steam Frame,
  Quest Pro, Pimax Crystal, Varjo, Vive Pro Eye ...), **off otherwise**.
- Options > Graphics (VR) row **Foveated Rendering**: Eye-Tracked (only with eye gaze) / Fixed / Off, switched at
  runtime, saved as `foveation=eye|fixed|off` in `ss4smvr/upscaler_menu.txt`. Default (reset) is the automatic choice.
- options.txt `foveation=-1|0|1|2` (auto, off, fixed, eye-tracked) overrides the saved choice at startup.
- Lab: `lab_fov_gaze=1` simulates an eye tracker looking up-left (tangents -0.4, +0.3).

## 5. Swing kills: the enemy's own death

- R30 dealt 2 x health + 10. Characters have an overkill threshold (`en_dswOverkillDamage`, `sosd_dswOverkillDamage`);
  damage that far past health can blow the enemy up instead of playing its death animation.
- Now the hit is just lethal: remaining health + 1, `DMT_PLAYERMELEE`. A damage multiplier that leaves the enemy alive
  gets another just-lethal hit (at most three). Gibs and kill credit stay native.
- Eligibility is unchanged from R30: only the target the native melee finder chose (the enemy that shows the prompt:
  hostile, non-decorative, in range and view, stunned where the enemy requires it, listed in
  `PlayerMeleeSelection.lua` for the held weapon) can be killed this way.

## 6. Lab (2026-10-27, SS4 lab copy, Pompeii start, still head, 1920x2160 per eye, RTX 4090, simulated Monado)

`tools/lab/perf_matrix.sh` (median GPU ms per frame on the XR queue) with left-eye dumps (`lab_eye_dump=150`).
Eye-tracked runs use `lab_fov_gaze=1` (gaze up-left; pattern centre 0.32 / 0.37 of the eye image).

| configuration | off | fixed | eye-tracked |
|---|---|---|---|
| two-pass, coarsest 2x2 (default) | 3.40 | 3.13 (−8 %) | 2.99 (−12 %) |
| two-pass, coarsest 4x4 (`foveation_coarse=4`) | 3.41 | 3.04 (−11 %) | 2.87–2.96 (−13 to −16 %) |
| single pass, 4x4 (record() pattern) | 3.33 | | 2.93 (−12 %) |
| DLSS phase B at 67 % (`vr_upscaler_menu=0 upscale_quality=1`), 2x2 | 3.70 | | 3.42 (−8 %) |

- **Images.**
  - The 4x4 periphery breaks smooth lighting gradients into visible steps.
  - Alpha-tested foliage gets blocky leaf edges with 4x4, because the discard runs per coarse fragment. In
    gameplay this shows on Pompeii's bushes at the 4x4 boundary.
  - With 2x2 as the coarsest rate, the eye image is indistinguishable from off at normal viewing scale, and the
    4x4 option saves only about 0.1 ms more. **2x2 is the default.**
  - In phase B the pattern follows the 0.67 render sub-rect (`render 0.67 x 0.67 of the target`) and stays at 2x2.
- **Eye tracking.** The pattern centre follows the (simulated) gaze per eye. The HUD stays sharp, because it is not
  a scene pass.
- **Single pass** renders into eye-size scene framebuffers in this build (no double-wide target was created), so
  one pattern per frame covers both eyes, at the average of the two eyes' centres. The double-wide path is kept
  for builds or settings that use the double-wide holder.
- **This scene is light for a 4090** (3.4 ms for two 4-MP eyes). The saving grows with shading cost: headset
  resolution, dense scenes, slower GPUs.
- **Prompted melee (Forum, `melee_prompt_lab.sh`):** 4 swing kills, each with one just-lethal hit (no extra hits
  needed). The prompt still reads "Melee" with no button. The lab keeps the next enemy in front, so the death
  itself is hidden in the captures.
- **VR suites with `foveation=1`:** SS4 21 passed and SM 21 passed, 0 failed (the layer now hooks every
  `vkCmdBindPipeline`).
- **Not verified in the lab:** the "Foveated Rendering" menu row. Keyboard Escape and the controller's Start
  (left secondary, seen in the input log) did not open the pause menu in this session. The row uses the same
  construction as the foliage-sway row, which was click-tested in R29.
