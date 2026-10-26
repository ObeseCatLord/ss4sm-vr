# R25: upscaler phase B, eye scene at a lower render size (Sam4.exe 616154, SamSM.exe 628492)

Static analysis only. Sources:
- Ghidra decompiles from a private scratch copy of `lab/ghidra_r19` (`tools/ghidra/DecompileList.java`). The decompiles
  are kept privately in `lab/r25/a1.c`..`a5.c`, and the address lists in `a*.txt`.
- capstone disassembly (`tools/disasm.py`), `callers.py`, `cvar_readers.py`, `port_sites.py`, `sig_at.py`.
- masked-signature ports with `sigscan.signature`/`find` in all four exes, and `tools/port_symbols.py`.

Nothing was run. No game file, cvar, user option, save or `src/` file was touched. Addresses are Sam4.exe VAs unless
they are marked **SM**. Notation:
- "gfx" = the engine graphics object (`*0x1422aa9b0`, SM `*0x142287b70`).
- "dp" = a drawport (layout in §2.1).
- "eye RT" = the target that device slot 0x028 returns for eye 1/2.
- `out` = eye RT size, `render` = `out / ratio`.

## 0. Answers

1. **The render size of a view has one source.** RenderView 0x140bc1cd0 calls the **MPix limiter 0x140b98fb0**
   (SM 0x140b99660) at **0x140bc2412** (SM 0x140bc2b03), passing the size of the current drawport's texture (the eye
   RT). The limiter has four callers in total: RenderView, the HDR allocator (at 0x140c88c52, SM 0x140c891d2) and two
   calls in 0x140151c20, a render-to-texture path. Other readers of `ren_fRenderingPixelRatio` do not size the scene.
   - **Override:** MinHook the limiter. Call the original, and when the return address is `0x140bc2412+5` and the
     device eye is 1 or 2, replace `*outW/*outH` with `round(out/ratio)`.
   - Return the original's `xmm0`, which is `ren_fRenderingPixelRatio`. RenderView scales the shadow-map sizes with
     it when it is below 1.
   - No cvar is written. [H]
2. **The engine already supports render < output** (this is what a user `ren_fRenderingPixelRatio` < 1 does). It works
   with two separate canvases:
   - The **3D canvas** is a pooled LDR render target of exactly the render size. RenderView allocates it through
     0x140e172f0, builds its dp in global **0x1422d4f98** (SM 0x1422b22f8) with box `{0,0,rw,rh}`, and makes it the
     current dp. It saves the eye dp in **0x1422d5198** (SM 0x1422b2358). PP writes this canvas.
   - The **HDR canvas** (0x1422d7358) is **not** reallocated per view. 0x140c88a10 sizes it as
     `min(M, limiter(M))`, where `M = max(current target, device slot 0x68)`. It stays allocated while it is between
     the needed size and the limited size. Each view then gets the **box `+0x10..+0x1c` = {0,0,rw,rh}** through
     0x140ee6640 → **0x140c891f0** (`hdr_subrect`).
   - With the override limited to the RenderView call site, `limiter(M)` in the HDR allocator stays unreduced. The
     HDR canvas therefore stays at the full eye size (given device slot 0x68 ≥ eye RT, §2.3) and the scene uses its
     top-left render-size sub-rect.
   - Every scene auxiliary target (depth, linear depth, the `0x140dff9e0` set) is sized from the HDR canvas
     **texture**, not from its box. [H]
3. **Where post-processing gets its size:**
   - PP reads `src` = the HDR canvas dp (its box gives the UV rect, `0x140b581e0`).
   - It writes `dst` = `view+0x288`, a copy of the 3D canvas dp made by 0x140ee6850.
   - Intermediates are sized from the src texture. The final pass draws into the dst box.
   - Afterwards execute-view 0x140bc0d20 rescales the 3D canvas into the eye dp, with **rescale pass 0x140bbf2c0**
     or **`gfx_copy` at 0x140bc0fb3**.

   **Yes, the mod can run PP at output size:**
   1. In the PP-dispatch detour, upscale into the canvas `[0,0,out]`.
   2. Call `hdr_subrect(1, outW, outH)`.
   3. Call the original PP with **dst = the saved eye dp (`*0x1422d5198`)** instead of `view+0x288`.
   4. Restore the box afterwards.

   The rescale cannot become a plain 1:1 copy: at equal sizes execute-view chooses mode 5 (CAS sharpening), and the
   3D canvas is render-sized. It must be **skipped** for that view, by MinHooking 0x140bbf2c0 (only caller:
   execute-view) and `gfx_copy` 0x140b5a770, filtered on `src == &3D canvas dp && dst == eye dp`. [H] for the
   mechanics; [M] that nothing else in the view's command list after the PP command depends on the current dp
   being the 3D canvas (§3.3).
4. **Jitter:** the projection (device slot 0x070) is computed before RenderView. The mod therefore has to use the
   **same render size it will return from the limiter**: `renderSize(eyeSize(), ratio)`, not the HDR canvas extent
   `g_color.w/h` as in phase A.
   - Tangent offset = `j·(r−l)/rw`, `j·(u−d)/rh`.
   - Halton phase count = `8·ratio²`: 18, 24 and 32 for 1.5, 1.7 and 2.0.
   - Motion vectors are computed over `rw×rh` (the shader already has `size` and `origin`).
   - DLSS/FSR get the canvas images with a render sub-rect of `rw×rh` at (0,0).

   The aspect error from rounding is under one render pixel and does not affect the frustum, because the projection
   comes from tangents. [H]
5. **Mip bias:** R23 already applies it. In phase B set
   `g_extraBias = log2(rw/outW) + upscale_lod_bias` (≈ −1.585 at 1.5, −1.766 at 1.7 and −2.0 at 2.0 with the default
   −1). The R23 window (execute-view entry → PP detour) is unchanged. [H]

**Smallest safe set.** Phase B adds three MinHooks: the limiter, `rescale_pass` and `gfx_copy`. It decodes two globals
from verified sites and changes the existing PP detour. It also needs a one-line change in the device:
slot 0x068 must return the eye RT size including `vr_fRenderingPixelRatio`. No code bytes are patched, and no
cvar, option or file is written. Every new site is unique in all four exes (§7).

## 1. The render size of a view

### 1.1 MPix limiter `0x140b98fb0` (SM 0x140b99660; `float (int w, int h, int* outW, int* outH)`)

Disassembly (Sam4, the same 84 masked bytes in SM, `sig_at.py`):

```
*outW = w; *outH = h;
if (ren_fMax3DRenderingMPix (0x1422ab2f4) > 0) { s = MPix / (w*h/1e6); if (s < 0.95) { s = sqrt(s); *outW *= s; *outH *= s; } }
*outW = max(1, (int)(*outW * ren_fRenderingPixelRatio (0x14224cdd4)));   // same for outH
return xmm0 = ren_fRenderingPixelRatio                                    // movaps xmm0, xmm2 at 0x140b99039
```

| caller | site (return = site+5) | input | use of the result | SM site |
|---|---|---|---|---|
| RenderView 0x140bc1cd0 | **0x140bc2412** | current dp texture `w = 0x140b57060(tex)`, `h = tex+0x24` (the eye RT for an eye pass) | 3D canvas size; `xmm0` < 1 scales `shb_pixScaledShadowDensity`/`MaxShadowSize` (0x1422e94d4/d0) | 0x140bc2b03 |
| HDR alloc 0x140c88a10 | **0x140c88c52** | `max(current dp texture, device slot 0x68)` | upper bound for the HDR canvas (§2.2) | 0x140c891d2 |
| 0x140151c20 (render-to-texture into the HDR canvas, `hdr_alloc(0x1e)` + `pp_dispatch(…,0x1e)`) | 0x140151d02, 0x140151d2e | backbuffer / its own dp box | not an eye pass | 0x1401528e0 (fn) |

Other `ren_fRenderingPixelRatio` readers:
- RenderView's clamp to 0.0625..16.
- 0x140053b70, 0x140156880 (a ×0.70711 hotkey cycle), 0x1401cc930, 0x140d16d80 (two `.pdata` chunks) and 0x140f4c9b0.

These either **temporarily write 1.0 and restore it** themselves (0x1401cc930, 0x140d16d80, and 0x140f4c9b0 as a
scoped object storing `&cvar`/old value) or do not size the eye scene. Because the mod never writes the cvar, all of
them keep seeing the user's value. [H for the list (cvar_readers), M for the purpose of each]

### 1.2 What RenderView does with the result (decompile `lab/r25/a1.c` lines 310-408)

1. `rt = gfx[0x38f]` (`gfx+0x1c78`, the current dp; for an eye pass, the eye dp that `Render(pi, rtDrawport, 1)` set).
2. It calls `limiter(texW, texH, &w, &h)` and sets `DAT_1422d4f98 = 0` (no 3D canvas yet). If `(w,h) != (texW,texH)` or
   `ren_bForceCAS`:
   - it clamps `w,h` to `gfx_pixMaxRenderTargetSize`;
   - it gets a pooled target `0x140e172f0(gfx, key ^ fmt, w, h, fmt(rt), …, 4, 20.0f, &created)` (log name
     "resized 3D canvas");
   - it builds dp **0x1422d4f98** with `dp_ctor 0x140b574e0(tex, box {0,0, rtBoxW·w/texW, rtBoxH·h/texH})` (snapped to
     full when within 0.99);
   - **`0x1422d5198 = dp_push 0x140b58e40(gfx,0,0)`** saves the eye dp;
   - `gfx->vt[0x258](gfx, &0x1422d4f98, 0,…)` makes the 3D canvas current.
3. `view_cmd_init 0x140ee6850(view, …, gfx[0x38f], …)` copies the current dp (now the 3D canvas) into **`view+0x288`**
   (0x38 bytes, box at `view+0x298..0x2a4`). It derives `view+0x2c0 = K / (fov·boxW/boxH)` from that box.
4. It calls `hdr_alloc(view+0x2e0 flags)` (§2.2), then `0x140ee6640(view)` → `hdr_subrect(flags & 0x20, boxW, boxH)` →
   HDR box = `{0,0,rw,rh}`.
5. It sizes the per-view scene targets from the HDR canvas *texture*: `0x140f4adf0` (fmt 0x13/0x14 linear depth),
   `0x140e43630`, and `0x140dff9e0` ×5 at `tex` size and half size.

So **render size = what the RenderView limiter call returns**, and everything downstream follows from it.
`end_view 0x140bc0970` (SM 0x140bc1060) pops the dp stack when `0x1422d5198 != 0` and clears it. Every view function
calls it after execute-view (0x14048f5eb in the player view, 0x1403dbd7f, and others).

### 1.3 Override (both builds)

MinHook `mpix_limiter`. In the hook, `_ReturnAddress()` is the caller's return address, because MinHook enters the
detour by a jmp.

```cpp
using LimiterFn = float(__cdecl*)(int w, int h, int* ow, int* oh);   // Win64: ecx, edx, r8, r9; returns xmm0
float __cdecl hookedLimiter(int w, int h, int* ow, int* oh) {
    const float r = realLimiter(w, h, ow, oh);            // keeps the engine's xmm0 (shadow scaling) unchanged
    void* ret = RETADDR();                                // _ReturnAddress() (MSVC) / __builtin_return_address(0) (MinGW)
    if (!phaseB()) return r;
    if (ret == g_rvRet) {                                 // rv_limiter_call + 5
        const int eye = deviceEye();                      // *(int*)(*vr_device_global + 0x18), as hookedPp
        if ((eye == 1 || eye == 2) && upscaling()) {
            uint32_t rw, rh; up::renderSize(uint32_t(w), uint32_t(h), rw, rh);   // replaces the user's MPix/ratio
            *ow = int(rw), *oh = int(rh);
            g_size[eye - 1] = {rw, rh, uint32_t(w), uint32_t(h), g_frame};      // armed for this eye's view
        }
    } else if (ret == g_hdrRet) {                         // hdr_limiter_call + 5: never let the HDR canvas shrink
        *ow = w, *oh = h;
    }
    return r;
}
```

The HDR-site identity applies to every view while phase B is enabled, not only eyes. Otherwise a scope view (eye 3)
with a user ratio < 1 would get a smaller limit than the eye-sized canvas, and `hdr_alloc` would reallocate it every
frame (§2.2, recreate rule). An E8 retarget of the two call sites is an equivalent alternative and needs no
return-address test. MinHook is simpler and is already linked.

## 2. Canvases and drawports

### 2.1 Drawport layout (0x38 bytes; `dp_ctor 0x140b574e0`, `hdr_subrect`, execute-view)

| off | meaning | evidence |
|---|---|---|
| +0x00 | texture* (width `0x140b57060(tex)` = `tex+0x20`, halved when `*(tex+0x18)+0x38 & 4`; height `tex+0x24`; format `0x140b57030`; samples `0x140b57020`; id `tex+0x28`) | 0x140b57060 disasm; RenderView, hdr_alloc |
| +0x08 | qword, 0 from the ctor | dp_ctor |
| **+0x10 x0, +0x14 y0, +0x18 x1, +0x1c y1** | **box** (viewport); size = `x1−x0`, `y1−y0` | dp_ctor (min/max of the rect), execute-view size compare, RenderView 3D-canvas box, PP light streaks `dst+0x18−dst+0x10` |
| +0x20 x0, +0x24 y0, +0x28 x1, +0x2c y1 | box clamped to the texture | dp_ctor; execute-view checks `+0x20/+0x24 == 0 && +0x28/+0x2c == tex size` before clearing borders |
| +0x30 | int 1 (valid) | dp_ctor |

`gfx->vt[0x258](gfx, dp, int, 0,0,0)` makes a dp current (`gfx+0x1c78`). `dp_push 0x140b58e40` pushes the current dp
(and the gfx matrices) on `gfx+0x360` and returns the previous dp.

### 2.2 HDR canvas `0x140c88a10` (SM 0x140c88f90; decompile `a1.c` 966-1164)

With the HDR flag (`param & 2`):
- `M = max(current dp texture, device->vt[0x68]())` (only slot-0x68 caller: 0x140c88c01, SM 0x140c89181).
- `L = limiter(M)` at 0x140c88c52, capped by `gfx_pixMaxRenderTargetSize`.
- **needed = (L < M) ? min(L, M) : M.**
- The canvas is (re)created with `rt_create 0x140b5e370(gfx, needed, 0, fmt, 0x26, 4)` ("Created HDR render-target of
  %1x%2" is logged) when any of these holds:
  - the dirty flag `0x140b9aac0(&0x1422d73a0)` is set;
  - canvas < needed;
  - canvas > L;
  - the AA sample count or the format changed.
- Then all four boxes of `0x1422d7358` and `0x1422d7320` are zeroed (0x140c88f00..0x140c88f22).

Consequences:
- Stock with a user ratio < 1: the HDR canvas is allocated at the *limited* size (the render size of M).
- With §1.3 (RenderView site overridden, HDR site identity): M = max(rw, slot 0x68) = slot 0x68, so **the canvas is the
  full eye size and stays allocated**. The scene uses only `[0,0,rw,rh]`.

### 2.3 Device slot 0x068 must cover the eye RT

`sMaxSize` (src/se4/vr_device.cpp) returns `recommended`, but the eye RT is `recommended × vr_fRenderingPixelRatio`
(`eyeSize`). R1 says the reference device returns "the same value as the eye RT actually created". With
`vr_fRenderingPixelRatio > 1` the canvas would be smaller than the output and phase B could not write it. **Return
`eyeSize()` from slot 0x068.** Slot 0x068 has one caller, 0x140c88c01 (`lab/device_calls_sam4.json`), so nothing else
changes. If the canvas is still smaller than the output (runtime check 2), fall back to the stock path for that frame.

### 2.4 `hdr_subrect 0x140c891f0` (SM 0x140c89770; `dp* (int singleSample, int w, int h)`)

- Picks the MSAA dp 0x1422d7320 when `singleSample == 0` and it exists, otherwise 0x1422d7358.
- If `w`/`h` ≤ 0, keeps the existing box size, falling back to the texture size.
- Clamps to the texture, keeping the aspect ratio.
- Writes `+0x18 = +0x28 = w`, `+0x1c = +0x2c = h`. When it picked the MSAA dp, it also writes the same values into
  0x1422d7358.
- Returns the dp, or NULL.

Phase B preconditions exclude MSAA (R13 §5), so pass `singleSample = 1`. Callers: `0x140ee6640`/`0x140ee67a0` (view),
RenderView, the PP command for view type 0x14 (0x140ed336a), and 0x140151c20.

## 3. Post-processing size and the rescale

### 3.1 Where PP gets its sizes

- The PP command `CFinalizeHDRCmd` (vtable 0x141f61550, ctor 0x140ed3190, execute **0x140ed32a0**, SM 0x140ed4370;
  sort key `+0x14 = 0xd0800`, added by 0x140edf4c0 from the view builder 0x140ee1c60) calls
  **`pp_dispatch(gfx, view+0x288, view+0x2e0, cmd+0x80)`**.
- `pp_dispatch` → `pp_apply 0x140ba2ae0(gfx, &0x1422d7358, dst, flags, …)` → handler → `pp_setup` stores
  `pp+0x140 = src` and `pp+0x148 = dst`.
- PP render 0x140aff360:
  - the light-streak scale is `(dst+0x18 − dst+0x10)/1280`;
  - the final pass 0x140afa930 samples src through `0x140b581e0(src)` (the UV rect of the src box);
  - its intermediates (FXAA etc.) are pooled at the **src texture size**;
  - it calls `gfx->vt[0x258](gfx, dst)` and draws into the dst box.
- At the end, `pp_dispatch` makes `dst` current, then runs either `0x140b5b080(gfx, *dst+0x28 id, 0)` or
  `gfx->vt[0x288](gfx, *dst, 0)`.

**Changing `src`'s box and passing another `dst` is therefore enough to move the whole PP to output size.** [H for
the final pass and pp_render; M that no other PP pass in 0x140afe1d0 or 0x140afc210 reads a render-size buffer of the
scene (e.g. depth) through the src box — runtime check 6.]

### 3.2 The rescale (execute-view 0x140bc0d20 tail, disasm 0x140bc0eba..0x140bc1137)

1. It loads `rdx = [0x1422d5198]` (**exec_eye_dp_load 0x140bc0eba**). If the value is non-zero, it compares the box
   sizes of the eye dp and of 0x1422d4f98:
   - equal → mode 5 (CAS);
   - ratio > 2.0625 → mode 6;
   - otherwise `ren_iRescalingFilter`.
2. It then either:
   - calls **rescale_pass 0x140bbf2c0(&0x1422d4f98, eyeDp, mode)** at 0x140bc1101 (`lea rcx` at **0x140bc10fa**), or
   - for filters 0/1, calls **`gfx_copy 0x140b5a770(gfx, &0x1422d4f98, 0, eyeDp, 0, filtered)`** at **0x140bc0fb3**.
     Before that it may clear the canvas borders of the 3D canvas.
3. Finally it calls `gfx->vt[0x258](gfx, [0x1422d5198], 1, …)` (0x140bc1109 → 0x140bc1137) and then draws the optional
   `0x1422d4f90` overlay into the eye dp.

rescale_pass has a single caller. `gfx_copy` has 30. Both are reached only through the two sites above, with
`src == &0x1422d4f98`.

**Suppression:**
- Hook both functions. Return without doing anything when all of these hold:
  - `g_ppWroteEye`;
  - `src == canvas3dDp`;
  - `dst == *eyeDpGlobal`.
- Set `g_ppWroteEye` in the PP detour after the original PP ran with dst = eye dp. Clear it at the entry and exit of
  `hookedExecute`.
- The final `vt[0x258](eye dp)` and the overlay still run, so the engine's state after the view is unchanged.

Do **not** clear 0x1422d5198 instead. execute-view would then make the 3D canvas current (`[rbp+0x7f]` path at
0x140bc112a), and the 0x1422d4f90 overlay would be lost.

### 3.3 Commands after PP (what could still draw into "the 3D canvas")

The view builder 0x140ee1c60 adds commands with sort keys. Those found in the builder:

| key (`cmd+0x14`) | command |
|---|---|
| 0xc000 | `0x140ece6a0` group |
| 0x10000 / 0x20000 | `CClearRenCmd` |
| 0x17a00 | depth-rendering resources |
| 0x19a00 | `CResolveFastLightsCmd` |
| 0x1d000 | 0x140ecab30 |
| 0x60000 | 0x140ed47b0 |
| 0x78000 / 0x82000 | `0x140edf7e0` groups |
| **0xd0800** | **`CFinalizeHDRCmd`** |
| **0xd0a00** | particle batching flush (0x140eda8a0, only with `prt_bAllowBatching`; execute 0x140edaf40 only calls 0x140d254b0 and clears a list slot) |

RenderView also adds a `CCallbackRenCmd` with key 0x70000.
- **Order by key is inferred** from the keys and the "Sort rendering commands" view command (vtable 0x141f63fd0); it is
  not traced. [M]
- No command was found that draws after the PP. If one did, it would now draw into the eye dp at full size, which is the
  current dp after `pp_dispatch`, instead of into the 3D canvas. Runtime check 7.

## 4. Projection, jitter, motion vectors, upscaler inputs

- The player view 0x14048dad0 computes the projection (0x140478a80 → device slot 0x070) **before** RenderView, so
  the render size must be computed by the mod at projection time:
  `up::renderSize(eyeW, eyeH, rw, rh)` with `eyeSize()` from the device. The limiter hook uses the same function on
  the eye RT texture size, which is the same number unless `gfx_pixMaxRenderTargetSize` caps it.
  - Log both values; runtime check 3.
- `jitterProjection`: replace `g_color.w/h` with `(rw, rh)` in `core::jittered(..., rw, rh)`. The phase count also
  changes: `up::jitter(frame, rw, outW, …)`.
- Motion pass: `pc.size = {rw, rh, n, f}`, `pc.origin = {0,0}`, dispatch `(rw+7)/8 × (rh+7)/8`. The output is in
  render pixels (DLSS `MVLowRes`, FSR scale 1), as in phase A.
- `up::Frame` needs `renderW/renderH` separate from the image extents:
  - **DLSS:** `InWidth/InHeight = rw/rh` (or the max of the quality range), `InRenderSubrectDimensions = {rw, rh}`,
    target = out. The colour and depth resources stay the full canvas images; the subrect base is (0,0).
  - **FSR 3.1:** `maxRenderSize = renderSize = {rw, rh}`, `upscaleSize = out`. The resource descriptions keep the
    canvas extents.
  - The **output** image is created at `out`, not at the canvas size. Add `TRANSFER_DST` to its usage for the
    fallback blit.
- Rounding: `rw/rh` differs from `outW/outH` by less than one render pixel, and the frustum is identical because it
  is built from tangents. `view+0x2c0` (LOD factor) uses the 3D canvas box aspect; this is the stock behaviour of a
  user ratio.
- R23 bias: `g_extraBias = up::mipBias(rw, outW) + g_lodBias`. `mipBias` returns `log2(rw/outW)`; `g_lodBias` is the
  option, −1 by default.

## 5. Recipe for `src/up/integrate.cpp` (phase B; phase A unchanged when the ratio is 1)

Keep the existing preconditions: two-pass stereo, `gfx_iAntiAliasingSamples = 1`, FXAA, motion blur and DOF off. These
are runtime-only, as in R13 §5.

1. **Symbols** (all in `manual.json` after this gate; `tools/symbols.txt` lists them):
   - `mpix_limiter`, `rv_limiter_call`, `hdr_limiter_call`, `hdr_subrect`, `rescale_pass`, `gfx_copy`,
     `exec_rescale_site`, `exec_eye_dp_load`, plus the existing `hdr_canvas`, `pp_dispatch` and `execute_view`.
   - Decode the globals and check the opcode bytes first. If a check fails, leave phase B off:
     - 3D canvas dp: `exec_rescale_site` must start with `48 8D 0D`; the global is `site + 7 + disp32(site+3)`, and
       `site+7` must be `E8` with a target equal to `rescale_pass`.
     - eye dp global: `exec_eye_dp_load` must start with `48 8B 15`; the global is `site + 7 + disp32(site+3)`.
     - both call sites must start with `E8`, with a target equal to `mpix_limiter`; save `site+5` as the return
       address.
2. **Gate** `phaseB() = upscaling() && up::ratio() > 1 && symbols ok`. The ratio is `kRatio[upscale_quality]`. Expose
   `up::ratio()`, or use `renderSize` on the output.
3. **Limiter hook** as in §1.3. `g_size[e] = {rw, rh, outW, outH, frame}` arms the eye.
4. **Projection:** in `jitterProjection`, for eye 1/2 with phase B, use the render size computed from `eyeSize()`
   (§4). Keep `ProjRec` per eye.
5. **`hookedExecute`:** at entry, `g_ppWroteEye = false`, and set the bias from §4 if the eye is armed. After
   `realExecute`, clear both.
6. **`hookedPp` (eye 1/2, `want` as now, eye armed this frame):**
   1. `rw,rh = dst+0x18−dst+0x10, dst+0x1c−dst+0x14` (`dst` = `view+0x288`, the 3D canvas copy).
      `eyeDp = *eyeDpGlobal`; `outW,outH` = its box.
   2. Require all of these, else run the stock path (only the RenderView override applies, so the engine upscales
      spatially and the image is still valid):
      - `(rw,rh) == g_size`;
      - `outW,outH == g_size.out`;
      - `g_color.w ≥ outW && g_color.h ≥ outH`;
      - `eyeDp` non-null.
   3. `t_phaseB = {eye, rw, rh, outW, outH}`. Then `dp = hdr_subrect(1, outW, outH)`, and check that the box of
      `dp` is `outW × outH`. If it is not, restore with `hdr_subrect(1, rw, rh)` and go stock.
   4. `realPp(gfx, eyeDp, flags, p4)`. The upscaler runs inside it, at the first canvas barrier (`onImageBarrier`,
      unchanged).
   5. `hdr_subrect(1, rw, rh)` restores the engine's box. Set `g_ppWroteEye = true`.
7. **`evaluateEye` with `t_phaseB`:**
   - colour, depth and motion use render size `rw×rh` over the canvas images;
   - output `outW×outH`;
   - copy back into the canvas at `{0,0,outW,outH}`;
   - **if the evaluate fails**, `vkCmdBlitImage` canvas `[0,0,rw,rh]` → own output `[0,0,outW,outH]` (LINEAR;
     RGBA16F blit is a required format feature), then copy into the canvas as above. PP then always sees a full
     image.
   - The barriers are those of phase A, with the copy extent = out.
8. **Rescale suppression:** MinHook `rescale_pass` `void(void* src, void* dst, int mode)` and `gfx_copy`
   `void(void* gfx, void* src, int a, void* dst, int b, int filtered)`. Each returns early when `g_ppWroteEye &&
   src == canvas3dDp && dst == *eyeDpGlobal`, and otherwise calls through. Count the skips.
9. **Device:** slot 0x068 returns `eyeSize()` (§2.3).
10. **Upscaler:** add `renderW/renderH` to `up::Frame` and use them as in §4. A size change re-creates the DLSS/FSR
    feature (already handled) and resets history.

Logging (first 4 occurrences, then once per second, as integrate.cpp does now):
- `limiter: eye %d %dx%d -> %dx%d (ret %p)`;
- `hdr limiter: identity %dx%d`;
- `pp: eye %d dst %dx%d eye %dx%d canvas %ux%u box %dx%d -> phaseB|stock(reason)`;
- `rescale skipped (pass|copy) eye %d`;
- the engine's own lines "Created HDR render-target of %1x%2";
- DLSS/FSR `eye %d %ux%u -> %ux%u`.

## 6. Runtime checks (SS4/SM lab runs need the user's go-ahead first; see the lab notes)

1. Limiter hook: about 2 RenderView hits per frame (eyes 2 then 1; eye loop order {2,1}, 0x1401dc520) with
   `out → out/ratio`. The HDR-site hits return the identity. There must be no hit from 0x140151c20 during play.
2. The engine logs "Created HDR render-target" once at the eye size (not the render size), and does not log it again
   every frame. `g_color` = eye size.
3. The projection-time render size equals the limiter output, for both eyes and at all three ratios.
4. In the PP detour, `dst` box = `rw×rh` and the eye dp box = `out`. After `hdr_subrect(1,out)` the box = `out`.
5. The eye image is correct and sharp. Compare it with phase A (ratio 1) at the same pose, as in R23 §6. There must be
   no stale or garbage frames, which would mean the rescale skip or the box switch is wrong. The skip counter equals
   the number of eye PP dispatches.
6. PP effects with depth or other render-size inputs: check light streaks, sun glare, zoom blur and the auto-exposure
   region at the image edges. Artefacts in the bottom or right region beyond `rw×rh` mean a PP pass reads a
   render-size buffer through the src box. The mitigation is a nearest upsample of the depth into the canvas depth
   before PP, using a mod compute pass.
7. Commands after PP: count render-pass begins on the eye RT framebuffer between the end of `realPp` and the return
   of `execute_view`. Expect 0 apart from the engine's own final dp switch and overlay.
8. Eye RT layout: the engine's `vt[0x288](eyeRT, 0)` at the end of `pp_dispatch` runs on the eye RT instead of the
   3D canvas. Check with Vulkan validation that the eye loop's TRANSFER_SRC transition and submit still see a valid
   layout.
9. GPU time per eye drops roughly with `1/ratio²` for the scene part. PP now costs output-size time. Record both.

## 7. Address table (Sam4 616154 / SamSM 628492)

All the function and site rows below are new in `docs/re/builds/signatures.json` (`tools/port_symbols.py`: unique in
Sam4, Sam4_Unrestricted, SamSM and SamSM_Unrestricted; no FAIL lines for the new names). The globals were decoded from
the ported sites.

| name | Sam4 | SamSM | how SM was found / note |
|---|---|---|---|
| `mpix_limiter` (float (w,h,int*,int*)) | 0x140b98fb0 | 0x140b99660 | sig 24; `sig_at` 84/84 |
| `render_view` | 0x140bc1cd0 | 0x140bc23c0 | sig 24 (R13) |
| `rv_limiter_call` (E8, ret +5) | 0x140bc2412 | 0x140bc2b03 | sig 24; decoded target = limiter |
| `hdr_limiter_call` (E8, ret +5) | 0x140c88c52 | 0x140c891d2 | sig 24; decoded target = limiter |
| `hdr_targets` (existing) / slot 0x68 call | 0x140c88a10 / 0x140c88c01 | 0x140c88f90 / 0x140c89181 | sig |
| `hdr_subrect` (dp* (int,int,int)) | 0x140c891f0 | 0x140c89770 | sig 64; instruction-by-instruction identical |
| view → HDR box (`0x140ee6640`) / view cmd init | 0x140ee6640 / 0x140ee6850 | 0x140ee7810 / 0x140ee7a20 | port_sites |
| `execute_view` (existing) | 0x140bc0d20 | 0x140bc1410 | R23 |
| `exec_eye_dp_load` (`48 8B 15` disp32) | 0x140bc0eba | 0x140bc15aa | sig 24 |
| `exec_copy_call` (E8 → gfx_copy) | 0x140bc0fb3 | 0x140bc16a3 | sig 32 |
| `exec_rescale_site` (`48 8D 0D`, then E8 at +7) | 0x140bc10fa | 0x140bc17ea | sig 24; call at 0x140bc1101 / 0x140bc17f1 |
| `rescale_pass` (void (dp*,dp*,int)) | 0x140bbf2c0 | 0x140bbf9a0 | sig 24; 88/88 |
| `gfx_copy` (void (gfx,dp*,int,dp*,int,int)) | 0x140b5a770 | 0x140b5acd0 | sig 24 |
| `end_view` | 0x140bc0970 | 0x140bc1060 | sig 32; 68/68 |
| pooled RT (3D canvas) | 0x140e172f0 | 0x140e18150 | `sig_at` 68/68 |
| dp ctor / dp push / dp UV rect | 0x140b574e0 / 0x140b58e40 / 0x140b581e0 | 0x140b579e0 / 0x140b59380 / 0x140b586e0 | port_sites |
| PP command execute / PP dispatch / PP apply | 0x140ed32a0 / 0x140c89010 / 0x140ba2ae0 | 0x140ed4370 / 0x140c89590 / 0x140ba31b0 | R13 |
| global: 3D canvas dp (0x38 bytes) | 0x1422d4f98 | 0x1422b22f8 | decoded at exec_rescale_site; SM box reads at 0x140bc15dc.. (0x1422b2308..) |
| global: saved eye dp (`dp*`) | 0x1422d5198 | 0x1422b2358 | decoded at exec_eye_dp_load; SM store 0x140bc2dd4 |
| global: HDR canvas dp / MSAA dp | 0x1422d7358 / 0x1422d7320 | 0x1422b4508 / 0x1422b44d0 | R13 (`hdr_canvas`) |
| global: gfx | 0x1422aa9b0 | 0x142287b70 | decoded (SM 0x140bc15c6) |
| cvars (read only) | MPix 0x1422ab2f4, PixelRatio 0x14224cdd4, Rescaling 0x14224d36c, ForceCAS 0x1422efdc4, MaxRT 0x1422ab3c0 | R13 §6 | harvest |

`tools/symbols.txt` gained `mpix_limiter`, `rv_limiter_call`, `hdr_limiter_call`, `hdr_subrect`, `rescale_pass`,
`gfx_copy`, `exec_copy_call`, `exec_rescale_site` and `exec_eye_dp_load`. `src/se4/symbols_gen.hpp` was not
regenerated. `render_view` and `end_view` are in `manual.json` for reference only.

Confidence:
- **[H]:** the limiter and its callers; the RenderView/HDR sizing; the drawport layout; the PP src/dst plumbing; the
  rescale sites; the SM ports.
- **[M]:** the command order after PP (§3.3); that no PP pass reads render-size scene buffers through the src box
  (check 6); the effect of the eye RT layout transition in `pp_dispatch` (check 8).
- **Not determined statically:** what 0x140151c20 is used for, and whether `0x1422d4f90` (the post-view overlay) is ever
  set during VR play.
