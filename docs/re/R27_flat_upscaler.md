# R27: DLSS/FSR in flat (desktop) play: projection, camera, main-view identity (Sam4.exe 616154, SamSM.exe 628492)

Static analysis only. Sources:
- Ghidra decompiles from a private scratch copy of `lab/ghidra_r19` (`tools/ghidra/DecompileList.java`, `-noanalysis
  -readOnly`). They are kept privately in `lab/r27/b1.c`..`b11.c` (address lists in `b*.txt`). Earlier decompiles reused:
  `lab/r25/a1.c` (RenderView, hdr_alloc, PP), `a2.c` (view_cmd_init, sub-views), `a3.c` (PP command), `a4.c` (Render).
- capstone disassembly (`tools/disasm.py`), `callers.py`, `riprefs.py`, `cvar_readers.py`, `class_vtables.py`,
  `port_sites.py`, `sig_at.py`, and the masked-signature ports in `tools/port_symbols.py`.

Nothing was run. No game file, cvar, user option, save, Steam userdata or `src/` file was touched. Addresses are Sam4.exe
VAs unless marked **SM**. Notation as in R25:
- "gfx" = the engine graphics object `*0x1422aa9b0` (SM `*0x142287b70`); `gfx+0x1c78` = current drawport pointer.
- "dp" = drawport (0x38 bytes, box at +0x10..+0x1c, R25 §2.1).
- "main dp" = the main flat view drawport global **0x1422737e0** (SM 0x142250880, §3.1).
- "view" = the 0x3a8-byte view command RenderView builds (global **0x1422d4d18**, SM 0x1422b2110).
- `P[16]` = a projection matrix, row-major floats; clip = P·(x, y, z, 1) in view space (x right, y up, −z forward).
- "flat" = no VR device: the device global 0x1422aad10 (SM 0x142287ed0) is NULL, `deviceEye()` returns −1.
- "slot 0" = the flat main view in the mod's per-view arrays (eyes stay 1 and 2).

## 0. Answers

1. **Projection.**
   - `0x140478a80` (SM 0x1404790b0) is `float* proj(player, float out[16], float aspect)`, with the aspect in xmm2.
     The player view passes the current dp box `w/h` (1.33333 if there is no dp).
   - Without a device it returns a **4×4 GL-style perspective matrix**, not a FOV struct:
     `persp_fov 0x141111460(out, fovRad, aspect, ren_fNearClipPlaneDistance, ren_fFarClipPlaneDistance)`.
     - `fovRad = clamp(0x140477aa0(player), ≤135)·0x140477e60(player)`, then `max(…,5°)`, then `·π/180`.
     - The FOV is a **horizontal FOV defined at 16:9**: `h = n·tan(fov/2)/(16/9)`, then
       `frustum 0x141111290(out, −h·aspect, h·aspect, h, −h, n, f)`.
     - Units: degrees in, radians to the builder, engine units (metres) for n/f. The defaults are 0.1 and 200000.
   - The main player view calls it once per frame from the **player view `0x14048dad0`** (SM 0x14048e0e0), at 0x14048db75.
     The full chain is in §1.2.
   - **Cleanest jitter point:**
     - Do not hook the projection function. Inside **`render_view`**, the matrix that the scene renders with is
       **`gfx+0xc2c`** (16 floats). RenderView copies it into `view+0x108` at **0x140bc295a**
       (`rv_proj_copy`, SM 0x140bc304b). That copy happens *after* the MPix limiter call at 0x140bc2412.
     - So in the existing limiter hook (return = `rv_limiter_call+5`), for the flat main view only:
       `P[2] −= 2·jx/rbw; P[6] += 2·jy/rbh` on `gfx+0xc2c`. Here rbw×rbh is the 3D-canvas box the engine derives from the
       limiter result (§1.5).
     - Restore the saved matrix after `end_view` 0x140bc0970.
     - This covers the player view **and** every camera/cutscene view. The HUD world-to-screen users (0x140478b60 and
       0x140480020, and the per-player view record 0x140bb3180) keep the unjittered matrix. [H for the order, M that
       nothing between the two sites rewrites `gfx+0xc2c` (§1.5)]
   - **Other scene views:**
     - Cutscene and camera-entity views use their own projection: an inline `persp_fov` in `camera_render_view`
       0x14062d810, or an ortho matrix. They still go through RenderView with the main dp (or a letterboxed copy of it)
       current, so the same jitter point and the same identity test apply.
     - The full list is in §1.4. The non-main ones (scope, precache, thumbnail, bakes, env grab, in-world monitors,
       mirror and cube sub-views, FMV) are excluded by the drawport test in §3.
2. **Camera per frame.** At the limiter hook (inside RenderView, for this exact view), read:
   - **`gfx+0xc70`**: 12 doubles, the world-to-view 3×4 matrix. Rows are 4 doubles: `R` (3) and `t` (1).
   - **`gfx+0xe48/+0xe50/+0xe58`**: the camera world position in doubles, `= −Rᵀt`.
   - **`gfx+0xc2c`**: the unjittered `P` (read it before writing the jitter).
   - **Derived values:**
     - tangents: `tl=(P2−1)/P0`, `tr=(P2+1)/P0`, `tu=(P6+1)/P5`, `td=(P6−1)/P5`;
     - near/far: `n=P11/(P10−1)`, `f=P11/(P10+1)`, computed in double;
     - pose: `worldFromView = inverse(W2V)`.
   - Every scene camera writes these fields: player, camera entity, precache. RenderView copies them into the view
     command (`view+0xa8` world-to-view floats, `+0x108` P, `+0x2e4` camera position).
   - The engine's own previous-frame world-to-view is at `gfx+0xd30` (12 doubles, written by the player and the camera
     entity). Use it as a cross-check only.
   - In the flat player view the placement source is `player vt+0xdf8` 0x1402a42a0. That calls `vt+0x340`
     0x1404794a0 mode 0, which reads `player+0xf20` (quat x,y,z,w + pos) when there is no vrd.
   - In VR the same `gfx+0xc70` holds the **eye** world-to-view (head ∘ slot 0x78). That would also resolve R13 runtime
     check 5. [H]
3. **Main flat view identity:**
   - **Render-view time** (render_view detour, entry). All of these must hold:
     - the device is NULL;
     - the thread is inside the player-views pass `0x14005be70` (hook it; `views+0xc` = view count, 1 = no split
       screen);
     - the current dp texture `*(void**)gfx[0x1c78]` equals the main dp texture `*(void**)0x1422737e0` (the backbuffer
       texture `*0x1422aa998`);
     - `P[14] == −1` (perspective).
   - Then record the view command `*0x1422d4d18` when the original returns.
   - **PP time** (`pp_dispatch` detour). Two conditions:
     - the return address is `ppcmd_pp_call+5` (0x140ed33b6). That excludes the FMV/image PP at `fmv_pp_call`;
     - `dst == recordedView + 0x288` (the PP command passes `view+0x288`).
   - **MPix limiter:** the return address is `rv_limiter_call+5`, with the render-view flag above set.
   - The destination is the main dp or its letterbox copy, both on the backbuffer texture. In phase B,
     `*0x1422d5198` (the pushed dp) points at it. [H for the structure, M for `dst − 0x288` = the recorded view
     (runtime check 5)]
4. **Flat vs VR pipeline:**
   - **HDR canvas size.** In flat the canvas is sized from the **backbuffer texture** and the current dp (no device
     slot 0x68):
     - `M` = the current dp texture, `L` = `limiter(backbuffer)`;
     - so the canvas is the window size at ratio 1;
     - but it is the **3D-canvas (render) size when a resized 3D canvas is current**.
   - **Phase B consequence.** Phase B must therefore make `M` = the backbuffer: swap `gfx+0x1c78` to the main dp around
     `hdr_targets` (§4.2). The R25 HDR-site identity alone is not enough in flat.
   - **Box vs canvas.** The view box can be **smaller than the canvas**: `gfx_fAspectRatio` letterbox, the view-size
     debug global 0x14224cdd0, or a cutscene camera letterbox. The upscaler must use the HDR dp **box**, not the image
     extent.
   - **What the mod controls:**
     - MSAA (`gfx_iAntiAliasingSamples` > 1) must be 1: the MSAA scene depth is not single-sample, and the canvas is
       resolved inside the PP;
     - FXAA, motion blur, DOF, auto exposure and CAS all run after the insertion point and need no forcing;
     - the user's `ren_fRenderingPixelRatio`/`ren_fMax3DRenderingMPix` are bypassed for the main view by the limiter
       hook;
     - no cvar is written.
   - **What stays unchanged:** the HUD is drawn after the view (player `vt+0xe10`) into the main dp, so it is outside
     the upscaler input as in VR. Details are in §4.

**Smallest safe set (flat).**
- **New hooks:** `render_view` (classify and record), `player_views` (in-pass flag and view count), and `end_view`
  (restore P).
- **Existing hooks, extended:** the limiter, `hdr_targets`, `pp_dispatch`, `execute_view` (LOD bias), and in phase B
  `rescale_pass` and `gfx_copy`.
- **Globals:** five are decoded from verified sites (main dp, backbuffer texture, view command, plus the R25 3D-canvas
  dp and saved dp).
- **Not touched:** no code bytes beyond R23's LOD-bias stubs, no cvar writes. All new symbols are unique in the four exes
  (§7).

## 1. Projection

### 1.1 `player_projection 0x140478a80` (SM 0x1404790b0), disasm + `lab/r27/b1.c`

```
float* player_projection(player* p, float out[16], float aspect /*xmm2*/) {
  if (dev = *0x1422aad10, dev && dev->eye(+0x18)) return dev->vt[0x70](out, dev->eye, near 0x14224d388, far 0x14224d38c);
  fov = min(0x140477aa0(p), 135.0f);          // 0x1415af668
  m   = 0x140477e60(p);                       // FOV multiplier
  fov = max(fov, 5.0f);                       // 0x141585430
  return persp_fov(out, m*fov*π/180, aspect, near, far);   // 0x14158bc08 π, 0x14158bc00 1/180
}
persp_fov 0x141111460 (SM 0x141111cf0): h = n*tan(atan(tan(fov/2)/K)) = n*tan(fov/2)/K,  K = *0x142256a48 = 16/9 (SM 0x142235368 = 16/9)
                                        frustum(out, l=-h*aspect, r=h*aspect, t=h, b=-h, n, f)
frustum 0x141111290 (SM 0x141111b20): out[0]=2n/(r-l) out[2]=(r+l)/(r-l) out[5]=2n/(t-b) out[6]=(t+b)/(t-b)
                                     out[10]=-(f+n)/(f-n) out[11]=-2fn/(f-n) out[14]=-1, rest 0
```

- **FOV source `0x140477aa0`** (SM 0x1404780d0), in degrees:
  - the player params `+0x90` (via 0x14019bcf0);
  - 60 when more than one local player;
  - `plr_fFOVOverride` (0x142248850) when > 0;
  - the held weapon's FOV (`weapon params +0x50c`) when not riding;
  - `plr_fFOVRideOverride`;
  - a speed-based riding blend;
  - a timed blend (+0x1078/+0x10b0 times);
  - finally a script/event hook (0x140ba6bd0) that may rewrite it.
- **Multiplier `0x140477e60`** (SM 0x140478490): event-overridable, defaults to the zoom blend
  `((1−p+0xfd0)(1−params+0x948)+params+0x948)·p+0xfcc`.
- **Matrix layout:**
  - The device path (`sProjection` → `core::engineProjection`) fills the **same layout**, which is why the VR jitter
    convention transfers unchanged.
  - The depth row is GL style (z in [−w, w]). `gfx_proj_derive` 0x140b603d0 (SM 0x140b60940) later rewrites the copy at
    `gfx+0xdf0` for the drawport sub-rect and to **reversed Z**:
    - `z' = −0.5·z + 0.5·w`;
    - this applies when the flag **0x14224cdc8** (SM 0x14222bb78; `.data` = 1 in both) is set;
    - this matches `core::depthDistance`. [H]

### 1.2 Who calls it for the main view each frame (flat)

```
main loop 0x14003a810: dp = *(backbuffer tex 0x1422aa998)+8 (default dp, 0x38 B) -> pi vt+0x50 Render 0x140053b70(pi,&dp,inVR=0)
 Render: 0x1422737a0 = dp; main dp 0x1422737e0 = dp (shrunk when view-size global 0x14224cdd0 < 1 or gfx_fAspectRatio
         differs by >1%)  [render_maindp_store 0x140053d6b]; dp_lock(&lk, gfx, &0x1422737e0, 2)  -> gfx+0x1c78 = &main dp
   player_views 0x14005be70(dp, pi+0xa8, views, size): for each local view (views+0xc = count; >1: sub-dp pushed)
     viewer vt+0x28 0x140415200 (vtable 0x14182e920; SM 0x140415130 in 0x141812308)
       -> 0x14048fad0 player render dispatch (SM 0x1404900c0, 76/80 bytes)
            camera active (player+0x1b80 handle or world+0x164 camera) -> 0x14062d7d0 -> camera vt+0x620 (§1.4)
            else/also -> player vt+0xe18 0x14048fc20 (SM 0x140490220) -> player view 0x14048dad0
                 0x14048db75: player_projection(p, &P, dpAspect) -> gfx+0xdf0 = P, gfx+0xc2c = P, gfx_proj_derive(gfx, gfx+0xdf0, gfx+0xc70)
                 placement = p->vt[0xdf8] (or replay cam 0x14049fd60) -> W2V = inv34(R(q),pos) -> gfx+0xc70 (doubles), gfx+0xe48 (cam pos)
                 0x140bb3180(view record) ; render_view 0x140bc1cd0 at 0x14048f5ab ; p->vt[0xe00] (adds 1st-person cmds)
                 execute: 0x14004dca0 -> execute_view(.., gfx, 1) (flat) | 0x140ac9440 (VR) ; end_view at 0x14048f5eb
            player vt+0xe10 0x14048f760: HUD, after the view, into the main dp
```

[H] Every arrow is a direct call or a vtable slot that was read from the image. The viewer slot was read from the
vtable, which the ctor 0x14040c4c0 stores.

### 1.3 The projection RenderView consumes

RenderView `0x140bc1cd0` (disasm `lab/r27` / R25 `a1.c` 405-529) runs in this order:
1. `rv_curdp 0x140bc23c3` reads `gfx+0x1c78`.
2. **`rv_limiter_call 0x140bc2412`**.
3. 3D canvas, `dp_push`.
4. View command ctor 0x140bc2772.
5. `*0x1422d4d18 = view` at **`rv_viewcmd_store 0x140bc2791`**.
6. **`rv_view_copy 0x140bc280c`** (`gfx+0xc70` doubles → floats).
7. `gfx+0xd30` previous view at 0x140bc288c.
8. **`rv_proj_copy 0x140bc295a`** (`gfx+0xc2c`, 0x40 bytes).
9. The stereo set `gfx[0x1d2..]` when single-pass.
10. `view_cmd_init 0x140ee6850`.

`view_cmd_init` writes:
- `view+0xa8` world-to-view (12 floats);
- `view+0xd8` previous world-to-view;
- **`view+0x108` = P**;
- `view+0x288` = copy of the current dp;
- `view+0x2c0` LOD factor = `(16/9)/(P[0]·boxW/boxH)`. The jitter does not change `P[0]`;
- `view+0x2e4` camera position = `−Rᵀt`.

`gfx+0xdf0` (the drawport/depth-adjusted copy) is **not** copied into the view. [H]

### 1.4 All scene-view paths (each calls `render_view`)

| path | Sam4 / SM | projection | current dp at RenderView | main view? |
|---|---|---|---|---|
| player view (`player_view_render`) | 0x14048dad0 / 0x14048e0e0; RV call `pv_rv_call` 0x14048f5ab / 0x14048fbb2 | `player_projection` | main dp | **yes** |
| camera entity (`camera_render_view`, vt+0x620 of the CCameraEntity family; cutscenes, spectator, world camera `world+0x164`) | 0x14062d810 / 0x14062d600; RV call `cam_rv_call` 0x14062f16c / 0x14062ef63 | inline `persp_fov(cam vt+0x658 rad clamp 0.01..3.12, dp aspect, vt+0x670 n, vt+0x678 f)` at 0x14062da73; **ortho** 0x141111ac0 when `cam+0x90 == 1` | main dp, or a **local letterboxed dp** (0x140b57f40) when `cam vt+0x648` aspect differs > 11 % (same backbuffer texture) | **yes** (cutscenes) |
| camera with a target handle `cam+0xa0` | 0x14062d7d0 → 0x1406e3890 (not ported, not traced) | – | expected: in-world monitor RT (CRemoteCameraViewer) | no (texture test) |
| second vt+0x620 implementation | 0x14071e570 / 0x14071dce0 (RV 0x14071fa0d / 0x14071f180) | device slots when VR | not traced | texture test |
| sniper scope view | 0x1407bf460 / 0x1407be9e0 (only when the device exists) | `persp_fov(scope fov)` | scope RT | no |
| precache marker camera | 0x1403db890 / 0x1403db810 | `persp_fov(cam, 4/3)` | backbuffer texture, **box /16** | no (outside the player views; box) |
| save thumbnail | 0x1401cc930 / 0x1401cc450 (calls 0x14048dad0 directly) | player | own RT; writes MPix=0 and view size 1 temporarily | no |
| bakes / env grab / 0x140bc02b0 via `0x140bc33b0` | 0x140bc33b0 / 0x140bc3aa0 | own | own RT | no |
| mirror/portal and cube sub-views inside a view | 0x140dd9850 (`view+0x2d0 = 1`), 0x140dbf250 (6 faces), 0x140da9dc0, 0x140de88c1 | derived from the parent | own view objects (`view_cmd_init` + builder 0x140ee1c60) | no (their PP dst ≠ main view+0x288) |
| FMV / full-screen image | `fmv_pp` 0x140151c20 / 0x1401528e0 | none | `pp_dispatch(…, 0x1e)` at `fmv_pp_call` 0x140151f66 / 0x140152c26 | no (return address) |

The HUD helpers that R13 §4.1 calls "other cameras" (0x1400d7de6, 0x1400ea915, 0x1400ff7b1, 0x140455944, 0x140997de8)
build immediate-mode 3D (HUD models) and never call RenderView. **Identify cutscenes** by the `render_view` return
address: `pv_rv_call+5` is the player, `cam_rv_call+5` is a camera entity. Reset the upscaler history when it changes.

### 1.5 Jitter: where and how

Options compared:
- **A (recommended).** In the limiter hook, after the original returns, when `ret == rv_limiter_call+5` and the
  render-view flag says flat main:
  - save `gfx+0xc2c` (64 B);
  - `P[2] −= 2·jx/rbw`, `P[6] += 2·jy/rbh`;
  - (jx, jy) in render pixels, x right, y down. This is exactly `core::jittered` on the tangents, so the existing
    `upscale_jitter_x/y` sign switches keep their meaning.
  - The values reach `view+0x108` at 0x140bc295a.
  - Restore the saved matrix in an `end_view` post-hook.
  - **The restore must be at end_view, not at render_view return.** RenderView's `dp_push` (0x140b58e40, after the
    limiter, only when a 3D canvas is created) **saves `gfx+0xc2c`/`+0xc70`**, and end_view's pop (0x140b585f0) writes
    them back. A restore earlier than end_view would be undone (the pushed copy is jittered). [H; `lab/r25/a2.c`
    2330-2400]
- **B.** Detour `player_projection` (flat branch) and offset `out[2]/out[6]`.
  - This misses cutscene cameras.
  - It jitters the HUD world-to-screen users (`0x140478b60` wrapper with 7 HUD callers, 0x140480020, the view record
    0x140bb3180).
  - It needs a predicted render size (the limiter runs later).
- **C.** Hook `persp_fov` and filter on the return addresses 0x140478b3c and 0x14062da78. This has the same issues as B,
  plus 16 callers.

Render box for the jitter, computed exactly as RenderView does (`lab/r25/a1.c` 333-397):
```
texW = 0x140b57060(tex), texH = tex+0x24 ; (ow, oh) = limiter output, clamped to gfx_pixMaxRenderTargetSize (0x1422ab3c0)
if (ow,oh) == (texW,texH) && !ren_bForceCAS:  rbw,rbh = box of the current dp           (no 3D canvas)
else: rbw = (boxW*ow)/texW (int64 truncation; = ow if rbw/ow > 0.99), rbh likewise
```
- Between the limiter and `rv_proj_copy`, the calls are: format queries, the pooled RT 0x140e172f0, `hdr_subrect`,
  `hdr free` 0x140c88f50, `dp_ctor`, `dp_push`, `set_dp` (gfx vt+0x258 = 0x140b5aaf0, which stores only drawport state),
  0x1409437e0, 0x140d52710, and the view ctor. None of them writes `gfx+0xc2c` in the code read. [M; runtime check 3]
- Skip the jitter when `P[14] != −1` (ortho camera).
- Advance the Halton index once per flat main view (one per frame).
- Phase count = `8·(out/render)²`, as in VR.

## 2. Camera pose, near/far, tangents for the motion vectors

Read at the limiter hook, before writing the jitter:

| datum | location | format / derivation | evidence |
|---|---|---|---|
| world-to-view | `gfx+0xc70` | 12 doubles, rows of 4 (`R00 R01 R02 t0`, …) = `inv34(R(q), pos)` of the camera placement | player view writes `*(gfx+0xc70)=m00 … +0xcc8=m23` from `0x1411467c0` (rigid inverse); camera entity writes `gfx[0x18e..0x199]`; RenderView reads at `rv_view_copy` (SM same offset) |
| camera position | `gfx+0xe48/+0xe50/+0xe58` | doubles `= −Rᵀt` | player 0x14048dad0, camera 0x14062d810, precache 0x1403db890 |
| previous world-to-view (engine) | `gfx+0xd30` | 12 doubles; the player stores it once per frame id (`p+0x1c24` cache vs `0x1422d4ac8`) | RenderView copies it into `view+0xd8` (0x140bc288c) |
| projection (unjittered) | `gfx+0xc2c` | `P[16]` §1.1 | §1.3 |
| tangents | from P | `tl=(P2−1)/P0, tr=(P2+1)/P0, tu=(P6+1)/P5, td=(P6−1)/P5` | frustum layout 0x141111290 |
| near / far | from P, in double | `n = P11/(P10−1)`, `f = P11/(P10+1)` (exact inverse of what the GPU used; the player's are `ren_fNear/FarClipPlaneDistance` 0.1/200000, the camera's are its own vt+0x670/0x678) | §1.1 |
| render size | limiter hook | `rbw×rbh` (§1.5) | – |

- `core::Pose worldFromView` = `{q = quat(Rᵀ), p = −Rᵀt}`, in engine units: worldScale 1 in flat.
- The axis convention is the one `worldEye()` already uses in VR (the placement's local axes are the view axes). The
  player view builds the same W2V from the `vt+0xdf8` placement.
- **Compute `currentToPrevious` in double** from the two 3×4 matrices: `C2P = W2V_prev · W2V_cur⁻¹`. Convert to float
  only at the end. World coordinates can be large.
- Units: metres. The depth is reversed Z (§1.1), the same as the VR path.
- Cross-checks for the lab:
  - `gfx+0xd30` equals the mod's previous W2V for consecutive player frames;
  - the per-player view record `0x140bb3180` (`project+0x20` object: `+0x80+i·0x40` P, `+0x180+i·0x30` W2V floats,
    `+0x240+i·8` box size, `+i·4` valid; i = player slot 0..4) holds the same unjittered values for the player view.
  [H for the formats, M for using `gfx+0xd30` as a history (the update is gated by the frame id)]

## 3. Identifying the main flat view

### 3.1 Facts

- The main loop renders into the default drawport of the **backbuffer texture** `*0x1422aa998` (SM 0x142287b60), which
  is read by `gfx_backbuffer_get` 0x140b5b220.
- `Render` copies that dp into **0x1422737e0** (SM 0x142250880) at `render_maindp_store`. It shrinks it centred when
  `0x14224cdd0` (SM 0x14222bb80; unnamed, `.data` 1.0, debug hotkey ×0.70711) < 1, or letterboxes it to
  `gfx_fAspectRatio`.
- It makes the copy current with `dp_lock` → `set_dp`.
- `set_dp` stores the **pointer** in `gfx+0x1c78`, or the texture's default dp if `dp+0x30 == 0`.
- The texture pointer at `dp+0` is therefore the stable identity. [H]
- **Split screen:** `player_views` pushes a stack sub-dp per view when `views+0xc` > 1 (layouts ½, ⅓, ¼). The texture is
  still the backbuffer, so the count is needed.
- **Precache** uses the backbuffer texture with a 1/16 box and runs outside the player-views pass.
- **Thumbnail, scope, bakes and monitors** use other textures.
- **Mirror and cube sub-views** have their own view objects and drawports.
- **The FMV PP** has no RenderView and calls `pp_dispatch` from `fmv_pp_call`. Its flags (0x1e) equal the per-view PP's
  flags, so flags cannot separate them. [H]

### 3.2 Tests

At each stage:
- **render_view entry** (thread-local, set for the duration of the call):
  `flatMain = !device && t_inPlayerViews && t_viewCount == 1 && cur && cur->tex == mainDp->tex && depth == 0`
  - `cur = *(dp**)(gfx+0x1c78)`;
  - `depth` is a recursion guard;
  - optionally `boxArea(cur) ≥ ¼·boxArea(mainDp)`.
- **render_view return:** `if (flatMain) g_flat = { view = *viewCmdGlobal, dst = view+0x288, ret = _ReturnAddress(),
  frame }`. Log the caller kind (player or camera).
- **limiter** (`ret == rv_limiter_call+5`): use the thread-local `flatMain` (we are inside render_view). This is where
  the render size, jitter and pose are handled.
- **hdr_targets** (called from RenderView at 0x140bc31cd, after the 3D canvas is current): use the thread-local
  `flatMain`.
- **pp_dispatch:**
  `ret == ppcmd_pp_call+5 && dst == g_flat.dst && g_flat.frame == frame && (flags & 1) == 0`.
  - `pp_cmd` 0x140ed32a0 passes `rdx = [cmd+0x38] + 0x288`; the commands' `+0x38` is the current view, as in the player
    `vt+0xe00` and the 0x140edf4c0 additions;
  - the PP command, the PP and the rescale run synchronously in `execute_view` on the same thread (0x14004dca0).
- **execute_view** (LOD bias): use a flag set at the flat render_view return and cleared at end_view.
- **rescale skip** (phase B): the R25 test `src == canvas3dDp && dst == *eyeDpGlobal` is unchanged. In flat
  `*0x1422d5198` is the main dp, or the camera's letterbox dp.

Other signals, for logging only:
- `view+0x2d0 != 0` marks a mirror/portal sub-view (0x140dd9850 writes 1; view_cmd_init writes 0);
- `gfx+0x2b8` bit 0 = an oblique clip plane is active in `gfx_proj_derive`;
- `*(*(gfx+0x1d78)+0x10)+0x3b0` is the render type (RenderView writes 0; 0x14 makes the PP command first copy an aux
  texture into the canvas, R13). [M]

## 4. Flat pipeline vs the VR eye path

### 4.1 Sizes and canvases

| item | VR eye | flat main view |
|---|---|---|
| target | eye RT (device slot 0x28), box = texture | backbuffer default dp copy 0x1422737e0; **box may be a sub-rect** (letterbox, view size) or a camera letterbox dp |
| `hdr_targets` input `M` | `max(current dp tex, device slot 0x68)` | **current dp texture only** (the slot 0x68 branch needs a device): backbuffer, or the **3D canvas** when RenderView made one current |
| `hdr_targets` limiter input (`hdr_limiter_call`) | `M` | **backbuffer texture** `*0x1422aa998` (0x140b5b220) |
| HDR canvas at ratio 1 | eye size | **backbuffer size** (= window/swapchain size; runtime check 1), capped by `gfx_pixMaxRenderTargetSize` |
| HDR box per view | `{0,0,eye}` | `{0,0,view box}` (0x140ee6640 → `hdr_subrect`) |
| 3D canvas | phase B only | phase B, and **stock whenever the user ratio/MPix < 1 or `ren_bForceCAS`** |
| when the pooled 3D canvas is newly created and smaller than the HDR canvas | – | RenderView **frees the HDR canvas** (0x140bc25fb → 0x140c88f50) so it is re-created at `M` |
| PP dst | `view+0x288` (eye or 3D canvas) | `view+0x288` = main dp (backbuffer) at ratio 1, else the 3D canvas |
| HUD | composition layer | drawn after the view by player vt+0xe10 into the main dp, outside the upscaler input |
| jitter source | device slot 0x70 (`sProjection`) | `gfx+0xc2c` in the limiter hook (§1.5) |
| camera | head world ∘ eye | `gfx+0xc70` (also valid in VR) |

### 4.2 Phase B in flat

The issue:
- With `ow < texW`, RenderView makes the 3D canvas (render size) current before `hdr_targets`. So `M` = render size.
- With the R25 identity at the HDR site (`L` = backbuffer), `needed = M` = render size.
- The free above then re-creates the canvas at the render size, so phase B could not write the output-size image into
  it.

The fix: in `hookedHdr`, when
`flatMain && phaseB && *(gfx+0x1c78) == canvas3dDp && *eyeDpGlobal`:
1. set `gfx+0x1c78 = *eyeDpGlobal` (the main dp);
2. call `realHdr`;
3. restore.

Why this is safe:
- `hdr_targets` reads `gfx+0x1c78` once (0x140c88a55 → its private 0x1422d7390, SM 0x140c88fd5 → 0x1422b4540).
  0x1422d7390 is used only inside it and is cleared by 0x140c88ff0.
- It uses only the texture size and the existence of that dp.
- The main dp texture and the 3D canvas have the same format (the canvas is created with `fmt(rt)`).

Result:
- `M` = backbuffer and `L` = backbuffer, so the canvas is the window size and stays allocated.
- After that, R25 §3 applies unchanged: `hdr_subrect(1, outW, outH)`, then PP with dst = `*eyeDpGlobal`, and the rescale
  is skipped.

[M; runtime check 7] Alternative: retarget the pooled-RT size at 0x140bc2554 so that the 3D canvas *texture* is the
output size while its box stays the render size. That needs one more hook. Not recommended.

### 4.3 Settings

| setting | Sam4 / SM | flat requirement | how (no persistent write) |
|---|---|---|---|
| `gfx_iAntiAliasingSamples` | 0x14224cdfc / 0x14222bbac | **must be 1** | Reasons: >1 creates the MSAA canvas 0x1422d7320, the depth is multisampled (the mod captures 1-sample depth only), and the resolve happens inside `pp_dispatch` before the evaluation barrier. Default: leave the upscaler off for the session and log `MSAA xN`. Optional `upscale_force_aa=1`: in-memory write of 1 at activation (the HDR canvas re-creates itself: `0x14224e1d0 != samples`), restored at shutdown. 60 readers, so a disp retarget is not practical. Whether the engine's config save would persist an in-memory value is **unknown** (record flag bit 2 is set on most user options; meaning not established). |
| `gfx_iFXAA` | 0x14224bee8 / 0x14222ada8 | none | runs in the final pass after tonemapping (0x140afa930 → 0x140b02230): extra blur only. Recommend off in the README. |
| `efx_bMotionBlur`, `efx_bDepthOfField` | 0x14224beac, 0x14224bea8 / 0x14222ad6c, 0x14222ad68 | none | PP after the insertion. In phase B they run at output size. |
| `efx_bAutoExposure` | 0x14224e1c8 / 0x14222cf78 | none | as VR (auto-exposure flag to the upscaler) |
| `ren_fRenderingPixelRatio`, `ren_fMax3DRenderingMPix` | 0x14224cdd4, 0x1422ab2f4 / 0x14222bb84, 0x1422884b4 | ignored for the main view | limiter hook returns `renderSize(tex)` (phase B) or **the identity (phase A)**. Without the identity in phase A, a user ratio < 1 gives DLAA at the reduced size followed by the engine's spatial rescale. |
| `ren_bForceCAS`, `ren_iRescalingFilter`, `gfx_iCASSharpness` | 0x1422efdc4, 0x14224d36c, 0x1422517e8 / 0x1422ccf54, 0x14222c110, 0x142230310 | none | at ratio 1 with CAS a full-size 3D canvas plus CAS rescale follows the PP (fine). Phase B skips the rescale, and with it CAS. |
| `gfx_fAspectRatio`, view-size global | 0x142273af0, 0x14224cdd0 / 0x142250a6c, 0x14222bb80 | none | gives a box smaller than the canvas: use the box |
| HDR mode (0x14224cdf8, unnamed) | R13 | ≠ 0 | LDR mode has no HDR canvas, so the upscaler stays idle (`g_color` never captured) |
| `gfx_bAllowPostprocessing` | 0x14224d200 / 0x14222bfb0 | 1 recommended | with 0, `pp_apply` does a `gfx_copy`. The first canvas barrier is then a copy (runtime check 6). |
| `tex_fLODBias` | 0x1422b3f70 / 0x142291130 | none | R23 stubs; add slot 0 |
| `hud_bShowRenderingResolution` | 0x1422d4b40 / 0x1422b1d00 | lab only | the user can show the engine's render size to confirm the limiter result |

## 5. Recipe for `src/up/integrate.cpp` (flat = slot 0)

Keep the VR path as it is. Every flat branch requires `deviceEye() == −1`, so VR is unchanged.

1. **Symbols** (in `manual.json`; `tools/symbols.txt` lists them).
   - New: `render_view`, `player_views`, `end_view`, `render_maindp_store`, `gfx_backbuffer_get`, `rv_viewcmd_store`,
     `rv_view_copy`, `rv_proj_copy`, `ppcmd_pp_call`, `pv_rv_call`, `cam_rv_call`, `fmv_pp_call` (log only).
   - Existing: `mpix_limiter`, `rv_limiter_call`, `hdr_limiter_call`, `hdr_targets`, `pp_dispatch`, `execute_view`,
     `hdr_subrect`, `rescale_pass`, `gfx_copy`, `exec_*`.
   - Decode and check each, or leave flat off with a log line:
     - main dp: `render_maindp_store` = `0F 11 1D` disp32 → `site+7+disp`;
     - backbuffer texture global: `gfx_backbuffer_get` = `48 8B 05` disp32 `C3` → `site+7+disp`;
     - view global: `rv_viewcmd_store` = `48 89 0D` disp32 → `site+7+disp`;
     - `rv_proj_copy` bytes = `0F 10 80 2C 0C 00 00` and `rv_view_copy` = `F2 0F 10 80 70 0C 00 00` (confirms the gfx
       offsets 0xc2c/0xc70);
     - `ppcmd_pp_call`, `pv_rv_call` and `cam_rv_call` start with `E8`, with targets `pp_dispatch` / `render_view`.
       Save `+5` as each return address;
     - gfx global = R25's decode.
2. **Slots.** Make the per-view arrays 3-wide: `g_proj[3]`, `g_hist[3]`, `g_size[3]`, `g_lastEval[3]`.
   - `slot = eye` for VR (1, 2), `slot = 0` for flat.
   - Replace `eye − 1` with `slot`.
   - Flat history is also reset when `g_flat.ret` changes (player ↔ camera) or the box changes.
3. **Hooks:**
   ```cpp
   thread_local int t_inViews, t_viewCount, t_rvDepth; thread_local bool t_flatMain;
   int hookedPlayerViews(void* dp, void* p2, void* views, int* size) {   // 0x14005be70
       t_viewCount = *(int*)((uint8_t*)views + 0xc); ++t_inViews; int r = realPlayerViews(dp, p2, views, size); --t_inViews; return r; }
   void hookedRenderView(void* world) {                                 // 0x140bc1cd0
       void* cur = *(void**)(gfx() + 0x1c78);
       const bool mainView = deviceEye() == -1 && flatEnabled() && t_inViews && t_viewCount == 1 && !t_rvDepth && cur &&
                             *(void**)cur && *(void**)cur == *(void**)g_mainDp;
       const bool saved = t_flatMain; t_flatMain = mainView; ++t_rvDepth;
       realRenderView(world);
       --t_rvDepth; t_flatMain = saved;
       if (mainView && g_flatArmed) { g_flat.view = *g_viewCmdGlobal; g_flat.dst = (uint8_t*)g_flat.view + 0x288;
           g_flat.ret = RETADDR(); g_flat.frame = g_frame; g_flatExec = true; }
   }
   void hookedEndView() { realEndView(); if (g_projSaved) { memcpy(gfx() + 0xc2c, g_projSave, 64); g_projSaved = false; } g_flatExec = false; }
   ```
4. **Limiter hook** (`ret == g_rvRet && t_flatMain`):
   1. `r = real(...)`, then `tex = *(void**)cur`, `texW = w`, `texH = h`.
   2. Phase B: `*ow,*oh = renderSize(w, h)`. Phase A: `*ow = w`, `*oh = h` (identity; the user scale is bypassed).
   3. Compute `rbw, rbh` (§1.5), `outW,outH = box(cur)`.
   4. `float* P = (float*)(gfx()+0xc2c)`. If `P[14] != −1`, set `g_flatArmed = false` and return `r`.
   5. Record `ProjRec[0]`: tangents and n/f from P (§2), `rw = rbw`, `rh = rbh`. Record the pose: W2V from
      `gfx+0xc70` (12 doubles). Keep the doubles for C2P.
   6. `++g_frame` (flat counter). Compute `up::jitter(g_frame, rbw, outW, jx, jy)`.
   7. Save `P`, then `P[2] −= 2·jx·scale/rbw; P[6] += 2·jy·scale/rbh`. Set `g_projSaved = true`, `g_flatArmed = true`,
      and `g_size[0] = {rbw, rbh, outW, outH, g_frame}`.
   8. Return `r` (the engine's xmm0 stays unchanged, R25).
5. **`hookedHdr`.** When `t_flatMain && phaseB && *(void**)(gfx()+0x1c78) == g_canvas3dDp && *g_eyeDpGlobal`, swap
   `gfx+0x1c78` to `*g_eyeDpGlobal` around `realHdr` (§4.2). The existing `g_hdrThread` image capture is unchanged.
6. **`hookedExecute`.** `slot = (eye==1||eye==2) ? eye : (g_flatExec ? 0 : −1)`. The bias logic is unchanged, indexed by
   slot.
7. **`hookedPp`:**
   1. `flat = deviceEye()==−1 && RETADDR()==g_ppCmdRet && dst==g_flat.dst && g_flat.frame==g_frame && g_flatArmed`.
   2. `want` for flat drops the `player_vr::localVrd()` requirement and keeps `(flags&1)==0`, the learned `g_ppFlags`,
      `!t_ppDepth` and `cb`.
   3. Phase B flat calls `ppPhaseB(..., slot 0)` unchanged:
      - `rw,rh` = dst box;
      - `eyeDp = *eyeDpGlobal` (main or letterbox dp);
      - `out` = its box.
   4. Phase A: `t_pendingEye = 0` is now a valid slot. Use a separate `t_pendingSlot = slot + 1` so that 0 still means
      "none".
8. **`evaluateEye(cb, slot)`:**
   - **Sizes.** Use the **HDR box**, not `g_res.w/h`: `boxW,boxH = hdr dp +0x18/+0x1c`, origin (0,0). Phase A: render =
     output = box. Phase B: render = `g_size[0].rw/rh`, output = `g_size[0].ow/oh`.
   - **Upscaler extents.** DLSS: `InRenderSubrectDimensions` and `Target` from these. FSR: `renderSize`/`upscaleSize`.
     Create the features at the canvas size (max), so that a letterbox change does not re-create them. Reset history on
     a size change.
   - **Pose.** Flat: `posed = g_proj[0].valid`. `c2p` comes from the stored double matrices (`W2V_prev · W2V_cur⁻¹`),
     not from `worldEye()`.
   - **Thresholds.** The teleport/cut thresholds use `worldScale = 1`.
   - **Motion pass.** Push constants as in VR: `size = {rw, rh, n, f}`, `origin {0,0}`, the current and previous
     tangents.
9. **Gate** `flatEnabled() = g_evaluate && up::active() != Off && gfx_iAntiAliasingSamples == 1 && symbols ok`. Phase B
   flat additionally needs the R25 phase-B symbols.

Logging (first 4, then once per second, as now):
- `flat: main dp %p tex %p box %dx%d backbuffer %ux%u views %d` (on change);
- `flat rv: %s cur %p box %dx%d` (player, camera or other, from `g_flat.ret`);
- `flat limiter: tex %dx%d -> %dx%d render box %dx%d out %dx%d`;
- `flat jitter %.3f %.3f P2 %.6f->%.6f P6 %.6f->%.6f`;
- `flat pose: pos %.2f %.2f %.2f (e48 vs -Rt diff %.4g) tan %.4f %.4f %.4f %.4f n %.3f f %.0f`;
- `pp: flat ret %p dst %p (expect %p) flags 0x%x hdr box %dx%d canvas %ux%u -> evaluate|stock(reason)`;
- `pp: fmv/other ret %p skipped`;
- the engine's `Created HDR render-target of %1x%2`;
- `rescale skipped (flat)`.

## 6. Runtime checks (flat; a lab run of SS4/SM needs the user's go-ahead first, see the lab notes)

1. The backbuffer texture size equals the swapchain extent. `Created HDR render-target` reports that size, once per
   resolution change.
2. Exactly one flat main RenderView per frame in gameplay (ret = `pv_rv_call+5`) and in cutscenes (`cam_rv_call+5`). No
   flat main view during the save thumbnail, level precache, or the main menu background (outside the player views).
3. After `realRenderView` returns, `view+0x110` and `view+0x120` (P[2], P[6]) equal the jittered values written at the
   limiter. After `end_view`, `gfx+0xc2c` is restored. If the image does not shift with `upscale_jitter_scale=1000`,
   also jitter `gfx+0xdf0` (row0 −= (2jx/rbw)·row3, row1 += (2jy/rbh)·row3) and recheck.
4. Pose: `gfx+0xe48` = `−Rᵀt` of `gfx+0xc70` to < 1e-6. `gfx+0xd30` of frame N equals `gfx+0xc70` of frame N−1 while
   moving. Static camera → MVs ≈ 0. Strafe → uniform horizontal MVs.
5. `pp_dispatch` from `ppcmd_pp_call+5` has `dst == g_flat.dst` once per frame. FMV playback hits only `fmv_pp_call+5`.
6. The first canvas barrier inside PP comes from `COLOR_ATTACHMENT_OPTIMAL`, with `gfx_bAllowPostprocessing` 1 and 0.
7. Phase B: after the M-swap, the canvas stays at the backbuffer size and `rescale skipped` counts equal the flat PP
   count. `hud_bShowRenderingResolution` shows the render size.
8. Letterboxed cutscene (camera aspect ≠ screen) and `gfx_fAspectRatio` set: the HDR box is smaller than the canvas, and
   the upscaler uses the box (no stretched or garbage borders).
9. Ortho camera (`cam+0x90 == 1`, if any level uses it): the view is skipped (stock), and logged.
10. MSAA > 1: the flat upscaler stays off and logs it.

## 7. Address table (Sam4 616154 / SamSM 628492)

New rows marked * are in `docs/re/builds/signatures.json` / `manual.json` and unique in Sam4, Sam4_Unrestricted, SamSM
and SamSM_Unrestricted (`tools/port_symbols.py`: no FAIL line for any new name, and none for Sam4/SamSM). Globals were
decoded from the ported sites.

| name | Sam4 | SamSM | how SM was found / note |
|---|---|---|---|
| `player_projection`* | 0x140478a80 | 0x1404790b0 | sig 32 |
| `persp_fov`* / frustum / ortho | 0x141111460 / 0x141111290 / 0x141111ac0 | 0x141111cf0 / 0x141111b20 / – | sig 24 / port_sites |
| FOV / FOV multiplier | 0x140477aa0 / 0x140477e60 | 0x1404780d0 / 0x140478490 | port_sites |
| `player_view_render`* (player view) | 0x14048dad0 | 0x14048e0e0 | sig 48 |
| `pv_rv_call`* (E8 → render_view) / end_view call | 0x14048f5ab / 0x14048f5eb | 0x14048fbb2 / 0x14048fbf2 | sig 24 |
| player vt+0xe18 / vt+0xdf8 / vt+0x340 (`player_view`) | 0x14048fc20 / 0x1402a42a0 / 0x1404794a0 | 0x140490220 / 0x1402a4060 / 0x140479b00 | port_sites |
| player render dispatch | 0x14048fad0 | 0x1404900c0 | prologue + 76/80 bytes [M] |
| viewer vtable / vt+0x28 | 0x14182e920 / 0x140415200 | 0x141812308 / 0x140415130 | decoded from the ctor (0x14040c4d9 / 0x14040c439) |
| `camera_render_view`* / camera dispatch | 0x14062d810 / 0x14062d7d0 | 0x14062d600 / 0x14062d5c0 | sig 48 / port_sites |
| `cam_rv_call`* | 0x14062f16c | 0x14062ef63 | sig 32 |
| second vt+0x620 / scope / precache / thumbnail | 0x14071e570 / 0x1407bf460 / 0x1403db890 / 0x1401cc930 | 0x14071dce0 / 0x1407be9e0 / 0x1403db810 / 0x1401cc450 | sig_at / port_sites / callers |
| RenderView wrapper (bakes, env grab) | 0x140bc33b0 | 0x140bc3aa0 | sig_at 28/28 |
| `player_views`* | 0x14005be70 | 0x14005bfe0 | sig 24 |
| `render_maindp_store`* → main dp | 0x140053d6b → 0x1422737e0 | 0x140053dcb → 0x142250880 | sig 24 |
| `gfx_backbuffer_get`* → backbuffer texture | 0x140b5b220 → 0x1422aa998 | 0x140b5b780 → 0x142287b60 | sig 32 |
| `rv_viewcmd_store`* → view command global | 0x140bc2791 → 0x1422d4d18 | 0x140bc2e82 → 0x1422b2110 | sig 24 |
| `rv_view_copy`* (`gfx+0xc70`) / `rv_proj_copy`* (`gfx+0xc2c`) | 0x140bc280c / 0x140bc295a | 0x140bc2efd / 0x140bc304b | sig 24; same offsets |
| RenderView: current dp read / hdr_targets call | 0x140bc23c3 / 0x140bc31cd | 0x140bc2ab4 / 0x140bc38c0 | port_sites |
| `ppcmd_pp_call`* | 0x140ed33b1 | 0x140ed4481 | sig 24 (`add rdx,0x288` before it in both) |
| `fmv_pp_call`* / `fmv_pp` | 0x140151f66 / 0x140151c20 | 0x140152c26 / 0x1401528e0 | sig 24 |
| `gfx_proj_derive`* / `proj_revz_read`* → reversed-Z flag | 0x140b603d0 / 0x140b604a1 → 0x14224cdc8 (=1) | 0x140b60940 / 0x140b60a11 → 0x14222bb78 (=1) | sig 24 |
| gfx API-ready flag (`gfx_proj_derive` gate) | 0x1422aa9a4 | 0x142287b68 | port_sites |
| set_dp (gfx vt+0x258) | 0x140b5aaf0 | 0x140b5b050 | port_sites |
| hdr_targets current-dp read → private global | 0x140c88a55 → 0x1422d7390 | 0x140c88fd5 → 0x1422b4540 | disasm |
| flat execute_view call (0x14004dca0) | 0x14004dcd0 | 0x14004dd30 | port_sites |
| view record (per player) | 0x140bb3180 | 0x140bb3860 | port_sites |
| camera position floats (player view) | 0x1422d5120/0x1422d5128 | 0x1422b2098/+8 | port_sites |
| view-size global (unnamed) | 0x14224cdd0 | 0x14222bb80 | decoded at 0x140bc2429 / 0x140bc2b1a |
| gfx fields (both builds, checked in the player view and RenderView) | `+0xc2c` P (logical), `+0xc70` W2V doubles, `+0xd30` previous W2V, `+0xdf0` P (API, adjusted), `+0xe48` camera position doubles, `+0x1c78` current dp | same | identical offsets in the 0x14048dad0 / 0x14048e0e0 disassembly (counts per offset match) |
| view command fields | `+0xa8` W2V, `+0xd8` previous, `+0x108` P, `+0x288` dp copy, `+0x2c0` LOD factor, `+0x2d0` mirror, `+0x2e0` PP flags, `+0x2e4` camera position | same (`+0x288` at `ppcmd_pp_call−0xd` in both) | view_cmd_init |

`tools/symbols.txt` gained `player_projection`, `persp_fov`, `player_view_render`, `pv_rv_call`, `camera_render_view`,
`cam_rv_call`, `player_views`, `render_maindp_store`, `gfx_backbuffer_get`, `rv_viewcmd_store`, `rv_view_copy`,
`rv_proj_copy`, `ppcmd_pp_call`, `fmv_pp_call`, `gfx_proj_derive` and `proj_revz_read`. `src/se4/symbols_gen.hpp` was
not regenerated.

Confidence:
- **[H]:**
  - the projection builder, layout, FOV semantics and reversed-Z flag;
  - the main-view call chain;
  - the RenderView order (limiter → push → copies);
  - the gfx/view field offsets in both builds;
  - the main-dp/backbuffer facts;
  - the dp_push save/restore of `gfx+0xc2c`;
  - hdr_targets' flat inputs;
  - the HUD after the view;
  - the SM ports.
- **[M]:**
  - nothing between the limiter and `rv_proj_copy` writes `gfx+0xc2c` (check 3);
  - the scene uses only `view+0x108`, not `gfx+0xdf0` (check 3);
  - PP dst = the recorded view + 0x288 (check 5);
  - the M-swap in `hdr_targets` (check 7);
  - the class identity of 0x14071e570 and 0x1406e3890;
  - the SM port of the player render dispatch.
- **Not determined statically:**
  - whether the backbuffer texture is the swapchain image or an intermediate;
  - whether an in-memory `gfx_iAntiAliasingSamples` write could be saved to the user config;
  - which levels use orthographic or multiple simultaneous camera views.
