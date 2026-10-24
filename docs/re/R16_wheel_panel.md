# Gate R16: weapon wheel on a hand panel with the world in stereo (Sam4.exe 616154, SamSM.exe 628492)

Static analysis only. Ghidra copy `lab/ghidra_r16` (of `lab/ghidra_r14`), decompiles `lab/r16/{a,b}.c`. SamSM was
checked with raw disassembly, `port_list.py`, vtable slot dumps and `callers.py`. Nothing was run. Addresses are
Sam4.exe PE VAs unless marked SM. Terms:
- "wheel": `CWeaponSelectionWheelHudElement`.
- "hud": the wheel's owning `CHud`, `he_phud` = wheel+0x08.
- "device": our `COpenXRDevice`, global `[0x1422aad10]` (SM `[0x142287ed0]`).
- "gfx": `[0x1422aa9b0]` (SM `[0x142287b70]`).
- "top": the menu manager's top screen `mgr+0x28` (R11).

Member names come from reflection (`meta_class.py --class`). Enumerator names come from .rdata strings.

## 0. Answers

1. **Why the headset view switches to the cinema screen.** Opening the wheel pushes a real menu screen.
   - 0x14012ba50 (SM 0x14012c3f0) runs `0x14094d9c0(pi, "CMSDummyWeaponSelectionController", 0, 0, params, 0)`.
     The params pass the wheel pointer as `"WeaponSelectionHudElement"` and an optional `"bShowCursor"`.
   - That class (vtable **0x141605e60**, SM **0x1415e87e0**, size 0x140) does not override `+0x1a8`. It keeps
     the CMenuScreen default, the shared `mov eax,1; ret` stub 0x1400472e0. Its `+0x1b0` is 0.
   - The VR view 0x1401dcbe0 (SM 0x1401dc7c0) therefore takes the cinema branch on every frame the dummy screen
     is top (R11 §2.1: `vr_bForceImmersiveMode == 0 && viewer+0xd0 == 0 && top->vt[0x1a8]()`).
   - Slow-mo (`0x1401b9150(player, 2)`) and the wheel's own fields play no part in this.
   - **Fix, local and non-replicated:** set `vr_bForceImmersiveMode` (0x142277ad0, SM 0x142254bd0) to 1 for the
     frames where `*(void**)top == dummy vtable`, and restore the user's value otherwise. Patching vtable
     `+0x1a8` is not an option because the stub is shared. The mod's menu pointer must also be off for those
     frames (§1.3).
2. **Where the wheel draws.** The HUD render 0x1400d7be0 (SM 0x1400d7cc0) calls the per-element render
   0x1400d6920 (SM 0x1400d6a00), which calls wheel `vt+0x80` = **0x1401298c0** (SM 0x14012a260).
   - It draws into **the gfx's current drawport** (`gfx+0x1c78`). In VR that is the eye RT, once per eye.
   - In immersive frames (`device+0x08` = 1) the element is drawn on a 3D head-following canvas. In
     cinema/desktop frames it is plain 2D.
   - Centre = drawport centre: base `vt+0x98` with reference (320, 240) → 0x1400dd1a0, safe area included.
   - Size ∝ drawport height × `hud_fWeaponWheelScale` (0x142247604, SM 0x1422284f4). The gadget list sits at
     `hep_vGadgetsListOffset`.
   - It draws **no** full-screen background. The world blur/dim is a separate **post-processing preset**
     (`hep_ppppPostProcessingPreset`, params+0xb0). Wheel `vt+0xc8` 0x140125850 pushes it with the fade weight
     into the world post-process list (§2.3).
3. **Only the wheel in a mod texture.** Re-run the engine's own HUD render once per frame into a mod drawport,
   with an element filter and the 2D path. This is the same nesting that the controller's single-pass HUD
   already uses for eye 2 (§3.1):
   1. lock: `0x140b57340(&lock, gfx, dp, 3, 1)`;
   2. clear the drawport (`gfx vt+0x268`, colour 0,0,0,0);
   3. `device+0x08 = 0` and call `0x1400d7be0(hud, …)`;
   4. unlock: `0x140b585f0(gfx, lock.flag)`.

   The detour on 0x1400d6920 lets only the wheel through in that pass and skips it in the eye passes. The RT is
   an engine holder created like the scope's (`holder_create` inside `eyes::beginCapture`), so the VkImage is
   captured by the existing vkCreateImage hook. The world stays in normal stereo.
4. **The gadget ring is the same element.** `he_awsgleGadgetsElements` (+0x50, 0x28 each) is filled in the open
   function from `hep_awsglsGadgetsSlots`. It is drawn in the same `vt+0x80` call together with the
   "ETRSNetricsa4.Gadgets" title and the tip text. It cannot be separated without splitting the draw.

Confidence: H for 1, 2 and 4 (decompile plus vtable slots in both builds). M for 3: every piece is an engine
idiom found in this build, but the combination has not been run. Alpha output is unverified (§6).

## 1. The cinema switch

### 1.1 Open and close
| step | Sam4 | SM | evidence |
|---|---|---|---|
| wheel step (vt+0x90): opens when `player+0x269c` bit0 is set and `wheel+0xc4` (dummy-screen handle) is empty, closes when the bit is cleared; fade `wheel+0xc0` ramps by dt/`hep_fFadeTime` (+0xb8) | 0x140127d60 | 0x140128700 | vt slot, decompile |
| open: builds weapon/gadget lists, slow-mo slot 2, pushes the dummy menu | 0x14012ba50 | 0x14012c3f0 | string refs in both |
| close: `dummy->vt[0xf0](0)`, clears bit0, commits the selection (R3) | 0x14012be40 | 0x14012c7e0 | sole call from the step in both |
| dummy init (vt+0x120): one invisible widget; `+0x138` = wheel handle; writes `wheel+0xc4` = own handle; `+0x13c` = bShowCursor | 0x140133a20 | 0x1401343f0 | vt slot |
| dummy OnCommand (+0x138): cmds 0x34/0x35 → wheel vt+0xc0 (ring cycle) | 0x1401339c0 | 0x140134390 | vt slot |
| dummy OnMouse (+0x168): every event → wheel vt+0xb0 (stub); evt 7 → wheel vt+0xb8(1), evt 10 → vt+0xb8(0) (mouse-mode click/preselect, 0x1401257a0) | 0x140133c00 | 0x1401345d0 | vt slot |
| dummy `+0x1a8` / `+0x1b0` / `+0x48` | 1 (stub 0x1400472e0) / 0 / 0 | same | slot dump |
| visibility (vt+0x60): `he_bIsVisible` (+0x10) = bit0 ∨ fade > 0.05 | 0x140128290 | 0x140128c30 | vt slot |

The wheel's own state never feeds the VR view. The only path is "the dummy menu is top". After release the
dummy screen closes at once, so the fade-out frames are already immersive.

### 1.2 Keeping stereo
The cvar is read only by 0x1401dcbe0, which runs inside RenderVR on the main thread. The mod already wraps that
call in `hookedRender` (`src/se4/vr_device.cpp`):

```
top = *(pi+0xb8) ? *(*(pi+0xb8)+0x28) : 0
wheelUp = top && *(void**)top == kDummyVt[build]
if (wheelUp) { saved = vr_bForceImmersiveMode; vr_bForceImmersiveMode = 1; }
renderVR(...)
if (wheelUp) vr_bForceImmersiveMode = saved;
```

This affects only the local view: the cvar is not replicated, and the menu is only opened by the local
player's controller.

With immersive forced, each eye's `Render(pi, eyeRT, 1)` also draws the menu state stack head-locked. For the
dummy screen that is its empty widget, plus probably the menu cursor when `bShowCursor` is set (mouse
selection mode; not checked).

### 1.3 The mod's menu pointer must ignore the dummy screen
`menuPointer()` (vr_device.cpp) acts whenever `top != 0`. While the wheel is open it would send mouse events
1/4/3/7/6 into the dummy screen, and the dummy forwards them to the wheel:
- a move to `vt+0xb0`;
- evt 7 to `vt+0xb8(1)`, which preselects a gadget in mouse mode.

The stale cinema quad would also give wrong hit tests. Skip the pointer when `top` is the dummy vtable.

## 2. The wheel's draw

### 2.1 HUD render and per-element render
`0x1400d7be0(hud, int* viewId, CEntity* viewEntity, float fov)` (SM 0x1400d7cc0).
- Callers: controller `vt+0x28` 0x140415200 (SM 0x140415130, vtable 0x14182e920 / SM 0x141812308) and player
  `vt+0xe10` 0x14048f760 (SM 0x14048fd60).
- Target: `gfx+0x1c78`, the current drawport, read on entry. `men_fScaleOverride` (0x142275d98) is saved and
  zeroed for the call.
- Flags it computes:
  - `hidden` = `player+0x7a6` bit0 clear. Elements then also need `vt+0x50`.
  - `menu` = no view entity ∧ top ≠ 0. Elements then need `vt+0x58`. The wheel returns 0 from both slots,
    but the normal path always passes a view entity, so `menu` = 0.
  - `vr` = `0x140b6af60()` = **device+0x08** (immersive; SM 0x140b6b540).
- If `vr` is set it builds the eye projection and view from device slots 0x070/0x078 and a head anchor P7.
  The anchor is slot 0x080 smoothed by `hud+0x90` and thresholds 0x142247540/44/48/4c.
- Element loop over `hud+0x50` (count +0x58): first pass over `hep_bExclusive` elements. If one draws, the
  others are skipped.
- Close-out: `gfx vt+0x258(dp,1)`, `0x140b94200`, text flush `0x1400ea890(hud+0xb0, gfx)` (SM 0x1400ea940),
  `0x140c6c040(gfx,1.0)`, `0x140b585f0(gfx,1)`.

`0x1400d6920(void** ctx, CHudElement* e)` (SM 0x1400d6a00). Context slots:
- [0] &hidden;
- [1] &menu;
- [2] viewId;
- [3] &viewEntity;
- [4] &cameraFlag;
- [5] &hudDrawport;
- [6] camera drawport copy;
- [7] &vr;
- [8] **&anchorP7** (quat xyzw, pos);
- [9] &dp;
- [10] hud.

Order of work:
1. Filters `vt+0x50` and `vt+0x58`.
2. `e+0x3c = *ctx[2]`; `vt+0x60` (visibility).
3. `he_bIsVisible`. When the camera flag is set, `he_hcrmRenderOnCamera` (+0x20; HCRM_NOT_RENDERED 0,
   _IN_NORMAL_DRAWPORT 1, _IN_CAMERA_DRAWPORT 2) selects the target through `gfx vt+0x258`.
4. If `vr`: model matrix = anchor ∘ `hep_qvVRPlacement` (params+0x18 quat, +0x28 pos) × `hep_fVRStretch` (+0x34),
   written to gfx+0xcd0…, and `dp+0x30` = 0. Otherwise `dp+0x30` = 1 (2D).
5. `e->vt[0x80](e, ctx[2], hud+0x10, hud+0x18)`.

### 2.2 Wheel `vt+0x80` 0x1401298c0 (SM 0x14012a260)
- **Input inside render:** in gamepad mode (`wheel+0xb8 == 1`) it reads `syscmdH±/P±` and `AltH±/AltP±` and
  calls 0x140125b20 (R3 §6). Today it therefore runs once per eye. The mod pass must run it exactly once per
  frame.
- Draws, all into the current drawport:
  - weapon slices `he_awsweWheelElements` (+0x40; 0x58-byte objects, vt 0x141604150, `vt+0x10`);
  - gadget slots +0x50 (`vt+0x10`);
  - text through `0x1400e8290(hud+0xb0, gfx, …)`: "ETRSNetricsa4.Gadgets" and the
    `ETRSHUD.WeaponWheel.GamepadGadgetTip` / `KeyboardGadgetTip` tips.
- Opacity comes from the fade `wheel+0xc0`.
- Geometry is built in `vt+0x40` 0x140126460 (SM 0x140126e00) from the params radii (+0x70..+0x84, rings
  +0x88/+0x98, `hud_fWeaponWheelBlendTransitionTickness`, `hud_slWeaponWheelElementSubdivision`).
- **Size:** `fVar7 = (HudScale / hud_fCustomHudScale) × hud_fWeaponWheelScale`, with
  `HudScale = hud_fCustomHudScale × layout(dp height) × safeArea.y` (0x1400dc930, 0x140b99060). So the wheel
  follows the drawport height, and `hud_fWeaponWheelScale` is the per-wheel knob.
- **Centre:** base `vt+0x98` 0x1400db320 → 0x1400dd1a0, reference point (320, 240) in a 640×480 frame mapped
  onto the drawport rectangle (dp+0x10..+0x1c) with the safe area. The gadget list is at
  `hep_vGadgetsListOffset[Alternative]` (+0x180/+0x188) from the centre (0x1401261e0, SM 0x140126b90). Its
  content value is unknown, so give the RT room on the sides (16:9).

### 2.3 Background
There is no background quad. Wheel `vt+0xc8` 0x140125850 (SM 0x140126250) is called for every element by
0x1400d7210 (SM 0x1400d72f0), from player code at 0x140468e42. When visible it calls
`0x140ba29b0(fade, 10000.0, preset+0x60, 0)` (SM 0x140ba3070), which queues the preset into a global,
spin-locked list (0x1422d4838) used by the world post-process. In stereo the world behind the panel will
therefore get the native blur/tint. To keep it clear, a detour of 0x140125850 can return early (local, cosmetic).

## 3. Rendering only the wheel into a mod RT

### 3.1 Engine idioms used
| idiom | Sam4 | SM | where seen |
|---|---|---|---|
| holder create/resize `(holder, gfx, w, h, fmt, 0, flags, f)` / get (`dp = *(get(h)+8)`) | 0x140c6ca30 / 0x140c6c900 | 0x140c6cfb0 / 0x140c6ce80 | cinema 0x1401d96a0 (fmt 1, flags 2, 1.0); scope.cpp (fmt 2) |
| drawport push `(gfx, dp, flags)` | 0x140b58e40 | 0x140b59380 | cinema clear |
| lock wrapper `(&{gfx,int}, gfx, dp, flags, int)`: push, then flush if `flags & 2` and dp+0x30 | 0x140b57340 | 0x140b57840 | controller vt+0x28 eye-2 HUD, fixed-screen 2D |
| drawport pop `(gfx, int)` (restores 0x250-byte stack entry at gfx+0x360/+0x368) | 0x140b585f0 | 0x140b58b00 | same |
| clear: `gfx vt+0x268(gfx, 1, 0, 0, -1, -1, &float4{0,0,0,0}, 1.0f, 0)` | vtable | vtable | 0x1401d96a0 |
| image hand-off: `gfx vt+0x288(gfx, *dp, 3)` (→ TRANSFER_SRC) | vtable | vtable | scope.cpp endFrame |

The engine already nests a second HUD render. In controller `vt+0x28` (0x140415cd8..0x140415da3; SM
0x140415ba1..), single-pass mode (`0x140b6b3d0(0)`, SM 0x140b6b9b0) renders the HUD for eye 1. It then locks
`slot 0x028(gfx, 2)` with `(3, 1)`, sets `gfx vt+0x60({1,0,0,0})` (eye selector, inferred), renders the HUD
again and pops. The mod pass is the same shape, with its own drawport and `device+0x08 = 0`.

### 3.2 Pass, per frame, render thread
Inside a detour of 0x1400d7be0 (SM 0x1400d7cc0), before calling the original for an eye, run the pass once per
frame when all of these hold:
- the wheel is visible;
- `hud == *(wheel+8)`;
- not already inside the pass.

```
lock = 0x140b57340(&lk, gfx, wheelDp, 3, 1)   // or push(gfx, dp, 0); clear; pop; then lock — the cinema order
gfx->vt[0x268](gfx, 1, 0, 0, -1, -1, zero4, 1.0f, 0)
s8 = device+0x08; device+0x08 = 0             // flat 2D layout (vr flag = 0)
inPass = 1; real_hud_render(hud, viewId, viewEntity, fov); inPass = 0
device+0x08 = s8
0x140b585f0(lk.gfx, lk.flag)
```

Per-element detour 0x1400d6920:
- in the pass, only an element whose `*(void**)e` is the wheel vtable is drawn; all others return 0;
- outside the pass, the wheel returns 0 (skipped in both eyes);
- everything else is unchanged.

If the content sets `hep_bExclusive` for the wheel, the rest of the HUD then stays visible while the wheel is
open (an acceptable change).

Arguments are those of the current eye call (viewId, viewEntity, fov). They change nothing in the 2D path
except the optional camera-drawport branch.

Wheel vtable: **0x141604220** (SM **0x1415e6ba0**; ctor 0x140124a10 / SM 0x140125410). The two builds
match slot for slot for `+0x00..+0xc8`.

## 4. Gadget ring
The gadget ring is the same element, the same draw call and the same drawport (§0.4). The gadget highlight
`wheel+0x88/+0x90` and ring cycling (`syscmdUp/Down`, `vt+0xc0`) are in R3 §6.

## 5. Ports
| item | Sam4 | SamSM | how |
|---|---|---|---|
| wheel vtable / ctor | 0x141604220 / 0x140124a10 | 0x1415e6ba0 / 0x140125410 | ctor `lea`; slot dump |
| wheel render vt+0x80 / step vt+0x90 / visible vt+0x60 / layout vt+0x40 / PP vt+0xc8 | 0x1401298c0 / 0x140127d60 / 0x140128290 / 0x140126460 / 0x140125850 | 0x14012a260 / 0x140128700 / 0x140128c30 / 0x140126e00 / 0x140126250 | SM vtable slots |
| open / close | 0x14012ba50 / 0x14012be40 | 0x14012c3f0 / 0x14012c7e0 | string ref; sole callers from step |
| dummy menu vtable / factory | 0x141605e60 / 0x140133840 | 0x1415e87e0 / 0x140134210 | factory `lea`; class record +0xc8 in both |
| HUD render / per-element / PP loop | 0x1400d7be0 / 0x1400d6920 / 0x1400d7210 | 0x1400d7cc0 / 0x1400d6a00 / 0x1400d72f0 | sig + callers |
| controller vt+0x28 / single-pass predicate | 0x140415200 / 0x140b6b3d0 | 0x140415130 / 0x140b6b9b0 | vtable 0x14182e920 / 0x141812308 |
| `device+0x08` reader (vr flag) | 0x140b6af60 | 0x140b6b540 | sig + disasm |
| VR view (cinema decision) | 0x1401dcbe0 | 0x1401dc7c0 | R11 |
| lock / push / pop / holder create / get | 0x140b57340 / 0x140b58e40 / 0x140b585f0 / 0x140c6ca30 / 0x140c6c900 | 0x140b57840 / 0x140b59380 / 0x140b58b00 / 0x140c6cfb0 / 0x140c6ce80 | SM controller vt+0x28 calls; R14 |
| PP push | 0x140ba29b0 | 0x140ba3070 | sig |
| cvars `vr_bForceImmersiveMode`, `hud_fWeaponWheelScale`, `hud_fCustomHudScale`, `hud_bUseAlternativeWeaponWheelForControllers` | 0x142277ad0, 0x142247604, 0x142247550, 0x1422475f4 | 0x142254bd0, 0x1422284f4, 0x142228440, 0x1422284e4 | harvest JSON |
| gfx / device globals | 0x1422aa9b0 / 0x1422aad10 | 0x142287b70 / 0x142287ed0 | disasm / manual.json |

Reflected offsets are the same in SM: wheel +0x10/+0x18/+0x40/+0x50/+0x78../+0xb8/+0xc0/+0xc4, params +0x18/+0x34/+0x38/+0xb0/+0xb8, dummy
+0x138/+0x13c, `player+0x269c` (SM vrd moves, the bit does not: SM wheel step reads the same field at the same
shape; M). The Unrestricted builds were not ported.

## 6. Unknowns and runtime checks
1. **Alpha.** Is the destination alpha the HUD writes into a cleared (0,0,0,0) target usable as layer alpha?
   This depends on the blend state. Log the VkFormat captured for holder fmt 1 and fmt 2, and pick one with an
   alpha channel. If alpha is wrong, fall back to an opaque dark backing (alpha 1) or to a luminance key in the
   copy shader.
2. Whether the menu cursor is drawn head-locked while the dummy screen is top in immersive mode (mouse mode
   only).
3. Content values: `hep_bExclusive`, `hep_vGadgetsListOffset`, `hep_qvVRPlacement`, `hep_fVRStretch`, and the
   preset at +0xb0. They decide the RT aspect and whether the world gets blurred.
4. Thread: the HUD render, and so wheel `vt+0x80` with its selection write, runs on the render/main thread
   today (twice per frame). The pass keeps that, once per frame.
5. Meaning of `gfx vt+0x60` (eye selector in the single-pass HUD) and of holder `flags`/last float. The pass
   only targets two-pass mode, which is the mod default.
6. `player+0x269c` in SM is assumed unchanged (field_refs not run for SM).

## Recommended implementation

1. **Symbols** (`docs/re/builds/*.json` manual entries, all four builds where they port):
   - `hud_render`, `hud_element_render`, `wheel_vtable`, `wheel_dummy_menu_vtable`;
   - `drawport_lock`, `drawport_pop`, `drawport_push`, plus the existing `holder_*`;
   - optionally `wheel_pp`;
   - cvars `hud_fWeaponWheelScale`, `vr_bForceImmersiveMode` (already present).
2. **Stereo while open** (`vr_device.cpp hookedRender`): compute `wheelUp` from
   `*(void**)(*(pi+0xb8)+0x28) == wheel_dummy_menu_vtable`. Force `vr_bForceImmersiveMode = 1` around
   `renderVR` and restore it afterwards. Skip `menuPointer()` while `wheelUp`.
3. **New module `src/se4/wheel_panel.cpp`** (shared, game differences only via symbols):
   - Detour `hud_element_render`. Wheel elements (`*(void**)e == wheel_vtable`) are drawn only in the pass;
     non-wheel elements are not drawn in the pass. Remember the wheel pointer.
   - Detour `hud_render`. On the first call per frame (frame flag reset in device slot 0x030), when
     `*(int*)(wheel+0x10)` is set, `*(wheel+8) == hud`, and the call is from an eye render (device+0x1c == 1,
     device+0x18 ∈ {1, 2}), run §3.2 into the mod RT.
   - The RT is created like the scope: alloc 0x90 + `holder_ctor` + `ref_addref`; `holder_create(holder, gfx,
     1600, 900, fmt, 0, 0, 0)` inside `eyes::beginCapture(4, …)` / `endCapture`, i.e. a new capture id 4 next
     to the scope's eye 3. `dp = *(holder_get(holder)+8)`.
   - In device slot 0x038, if the pass ran: `gfx vt+0x288(gfx, *dp, 3)`, then `eyes::submitted(4)`, as in
     `scope::endFrame`.
4. **XR layer** (`src/vk`, copy `scope_layer.cpp`):
   - Copy the image into its own swapchain (with alpha) and submit an `XrCompositionLayerQuad` with
     `XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT` (unpremultiplied unless check 1 says otherwise).
   - Pose: the opening hand's grip at open time, placed 25–30 cm forward and slightly up, facing the head
     (option: follow the hand). Size about 0.45 × 0.25 m (option).
   - The opening hand is the hand whose button the mod maps to the virtual-pad LB.
   - Submit while `he_bIsVisible`. The fade is already in the image.
5. **Selection** stays native (R3 §6): feed `syscmdH±/P±` from the opening hand's stick, or from the hand
   offset projected on the panel plane. Commit on release (0x14012be40). Nothing is replicated beyond what the
   desktop wheel already sends.
6. **Optional:**
   - Detour `wheel_pp` 0x140125850 / SM 0x140126250 to keep the world unblurred (option `wheel_blur`).
   - Cheaper alternative without an RT: leave the wheel in the eye passes and, in the per-element detour,
     replace `ctx[8]` (the HUD anchor P7) for the wheel only with `handPanel ∘ inverse(hep_qvVRPlacement)` in
     device play space (the same space as slot 0x080/0x0b8). The engine then draws it in stereo at the hand.
     Its size follows `hep_fVRStretch` and the canvas mapping, which were not traced. Use it as a lab
     prototype or a fallback if the alpha check fails.
7. **Lab checks**, in order:
   1. With the wheel held, the log shows `vr_bForceImmersiveMode` forced, the world is in stereo, and slot
      0x080 is not called for cinema placement.
   2. Capture 4 records an image and the copied panel shows the wheel and gadget list centred.
   3. The eye images contain no wheel.
   4. The selection highlight moves once per stick deflection and is no faster than before (it was evaluated
      twice per frame).
   5. Repeat on SM.
