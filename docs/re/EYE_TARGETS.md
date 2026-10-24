# Eye render targets and stereo paths (Sam4.exe 616154; ported to all builds via signatures.json)

## Engine VR render, `vr_eye_render` 0x1401dc520 (viewer method; viewer = viewport+0x360)
- Size: base helper 0x140b6aca0 = device slot 0x60 × `vr_fRenderingPixelRatio`. Sets device+0x1c = 1.
- **Two-pass** (`vr_iUsingSinglePassStereo` < 1 or device+0xb44 ≠ 0): for eye 2 then 1:
  `rt = dev->vt[0x28](gfx, eye)`; if rt: push eye render state (0x1401c2da0, 1/2), device+0x18 = eye,
  render the view into rt (0x1401deaf0), gfx `vt+0x310`.
- **Single-pass**: render state 3; unless `0x140b6b3d0(0)`: `holder_create(viewer+0x38, gfx, w, h, 2, 0, 4, 0)`
  (flag 4 = double width) and render once into `*(holder_get(holder)+8)`; then copy each half into
  `dev->vt[0x28](gfx,1/2)` with `gfx_copy` 0x140b5a770 (right half via 0x14060b2a0). device+0xb1c selects
  non-HMD stereo outputs (keep 0).
- Then (0x1401dcbe0) gfx `vt+0x288(gfx, *tex, mode)` per eye, mode 3 for Vulkan, and device slot 0x18.

## Render-target holders
- Viewer ctor 0x1401d7d90 creates four holders at viewer+0x30..+0x48:
  `engine_alloc(0x90, tag)` → `holder_ctor` 0x140c6c6b0 → `ref_addref(holder, &slot)` 0x1410c6af0.
- `holder_create` 0x140c6ca30 `(holder, gfx, w, h, fmt=2, 0, flags, 0)` → engine texture id stored at
  holder+0x7c (−1 = none); `holder_get` 0x140c6c900 returns the texture wrapper; device eye RT = `*(wrapper+8)`
  (same shape as Fusion's `0x24e47d0`, which returns `rt+8` from gfx `0x1fb28a0(gfx,w,h,0,2,0,0)`).
- `COpenXRDevice` slot 0x28 builds two holders the same way and calls `holder_create(..., 2, 0, 0, 0)`.

## Vulkan transition before submit: SS4 CGfxDevice(VLK) vtable 0x142093888 (127 slots)
- `vt+0x288` = 0x14136f930 `(gfx, tex, mode)`: mode 1 → layout 2/3 (color/depth attachment), 2 → 5
  SHADER_READ_ONLY, **3 → 6 TRANSFER_SRC_OPTIMAL** (stage 0x1000 TRANSFER), 4 → 7 TRANSFER_DST. Only active
  when the unnamed global 0x14224ce54 ≠ 0 and the gfx has a current command buffer (+0x1ea8) — confirm at runtime.
- Other slots of that vtable: 0x60 0x141385730, 0x98 0x14138ff20, 0x1c0 0x14137e790, 0x2c8 swap 0x141384240.
- Fusion's base `vt+0x288` is a no-op; SS4 added the Vulkan/D3D12 barrier path.

## Mod pipeline (implemented, untested at runtime)
Device slot 0x28 brackets `holder_create` with `eyes::beginCapture` so `vkCreateImage` on that thread with
the eye extent and COLOR_ATTACHMENT usage is recorded as the eye's VkImage (+ format). Slot 0x18 marks the
eye submitted. The present hook copies both eye images (TRANSFER_SRC_OPTIMAL) into a 2-layer XR swapchain
(raw copy into the _SRGB twin for 8-bit UNORM, blit otherwise) and submits a projection layer with each
eye's horizontal FOV and the shared vertical FOV, falling back to the mirror quad until both eyes arrive.
Runtime checks pending: synchronous vkCreateImage in holder_create; eye layout at present; format.
