# R1 — VR device vtable and object layout (Sam4.exe 616154)

What a replacement `COpenXRDevice` stored at `qword [0x1422aad10]` (SamSM: `0x142287ed0`, not yet
re-verified slot by slot) must look like: every vtable slot SS4 calls, the MSVC x64 signature it is
called with, what Fusion 2017's `CSteamVRDevice` / `CEmulatedVRDevice` do in that slot, and the base
fields SS4 reads or writes directly on the object.

## Evidence and method

| source | produced by | file |
|---|---|---|
| SS4 call sites, register args, direct field accesses | `tools/device_calls.py` (tracks the pointer loaded from the global, its vtable register, branch merge, clobbers) | `lab/device_calls_sam4.json` |
| Fusion call sites (same analysis, SysV ABI) | `tools/elf_device_calls.py`, global `0x41f2d50` | `lab/device_calls_fusion.json` |
| Fusion cvar / script-function names | `tools/elf_harvest.py` (same 64-byte registration records as SE4) | `lab/fusion_symbols.json` |
| decompiles | Ghidra `DecompileList.java` | `lab/fusion_slots_decomp.c`, `lab/fusion_helpers_decomp.c`, `lab/fusion_helpers2_decomp.c`, `lab/sam4_devcallers_decomp.c`, `lab/sam4_helpers_decomp.c` |

Fusion addresses are ELF VAs (non-PIE), SS4 addresses are PE VAs. "Fusion #k" is the Itanium vtable
index from `lab/fusion_slots.txt`; SS4 offset = (k−1)·8 for k ≥ 2.

**The −8 mapping holds over the whole Fusion range.** For every slot that has callers in both games,
the Fusion caller set at offset X and the SS4 caller set at X−8 are the same functions (same script
names where harvested): GetTrackedDevicesCount 0x188→0x180, GetTrackedDeviceFlags 0x190→0x188,
GetDeviceSerialNumber 0x1a0→0x198, GetPressedControlsForDevice 0x1a8→0x1a0, CVRCameraEntity::
IsTriggerPressed* 0xd8→0xd0, CMSGSCameraCalibration::* 0x108→0x100, vrGetDeviceAPI 0x150→0x148,
vrGetDeviceName 0x118→0x110, Vulkan extension queries 0x1b0/0x1b8→0x1a8/0x1b0, explicit timing
0x1d0→0x1c8 (both in the Vulkan/D3D12 present paths). Fusion has 63 slots (#0..#62, last at +0x1f0 →
SS4 0x1e8). SS4 slots 0x1f0..0x210 have no Fusion counterpart.

Correction to R1_device_seam.md: `ShowSteamVRActions` is not a Fusion slot (Fusion has no
`vrShowSteamVRActions` script function and its vtable ends at +0x1f0); SS4 0x1f0 is the first
SS4-only slot.

Confidence: **H** = Fusion body and SS4 call-site arguments agree; **M** = one side, or semantics
inferred from use; **L** = guess, kept only where marked.

## Calling convention (all slots)

- `this` in RCX. Integer/pointer args by position in RDX, R8, R9, then `[rsp+0x20]`…; a float in
  position n uses XMM(n−1) (position 2 → XMM1, 4 → XMM3). Floats return in XMM0, ints/pointers in RAX.
- MSVC member functions return any struct/class **through a hidden pointer in RDX** (args shift right
  by one) and return that pointer in RAX. SS4 callers read the hidden buffer (e.g. `0x140b6aca0` reads
  `[rbx]` after `call [rax+0x60]` with `rbx=rdx`), so the implementation must fill `*ret` and return it.
- `CString` is one `char*` (8 bytes). A returned CString must be built with the engine constructor
  `0x1410bfac0 (CString* this, const char*)` because callers destroy it with `0x1410bfce0`, which frees
  through the engine allocator (`0x1410c56f0`). Empty-string singleton: `0x1422566b8`.
- Placement = 7 floats `{qx,qy,qz,qw, px,py,pz}` (28 bytes). Quaternion order from Fusion #17
  (`ret[3]` is the scalar term of the Hamilton product). Positions are metres ÷ `vr_fWorldScale`
  (SS4 `0x14224cb98`; Fusion `0x396ca78`, divided in #16/#17/#24/#25).
- Matrix3x4 = 12 floats row-major `[r00 r01 r02 tx | r10 r11 r12 ty | r20 r21 r22 tz]`
  (SS4 `0x1401d9e40` reads translation from indices 3/7/11). Matrix4x4 = 16 floats row-major, built by
  SS4 `0x141111290 (out, l, r, t, b, n, f)` / Fusion `0x22b9d30`: m0=2n/(r−l), m2=(r+l)/(r−l),
  m5=2n/(t−b), m6=(t+b)/(t−b), m10=−(f+n)/(f−n), m11=−2fn/(f−n), m14=−1, rest 0.
- Eye index used by the engine: 1 = left, 2 = right (Fusion maps to OpenVR `eye-1` / `param!=1`),
  3 = mixed-reality/spectator camera, 0 = none. Controller role: 0 = right hand, 1 = left hand,
  2 = aux tracker (SS4 `CVRCameraEntity::IsTriggerPressedLeft` 0x14071da50 reads role 1,
  `…Right` 0x14071dac0 role 0; Fusion `CPlayerVRData` uses the same).

## Object lifetime and layout

- Size: Fusion `CVRDevice` base is **0xb70** bytes (both derived classes start their own fields at
  +0xb70: CSteamVRDevice `IVRSystem*` at +0xb70, CEmulatedVRDevice window size at +0xb70; base ctor
  `0x1fe06a0` / dtor `0x1fddad0` touch up to +0xb68). Factories: `0x24e6cc0` allocates **0x2e80**
  (CSteamVRDevice), `0x24e2d30` allocates **0xce8** (CEmulatedVRDevice); both store the factory
  argument (graphics API) at +0xb48 and call slot #2 (Init). SS4 accesses base fields up to +0xb68, so
  keep ≥ 0xb70 bytes of base area and put private state after it.
- Shutdown `0x140b6af00`: `vt[0x0](dev, 0)` (scalar-deleting dtor with flags 0 = destruct only), then
  `0x1410c55f0(dev)` — the engine allocator's free, which subtracts a header (`param−DAT_1423a0a84`).
  So the object must be allocated with the engine allocator (`0x1410c52f0(size, tag)`, used as `new`
  at `0x14043e330`) **or** the global must be cleared before `0x140b6af00` runs. The dtor must not
  free memory itself when flags==0.
- No SS4 code stores the device pointer anywhere else (device_calls: zero non-stack stores of a
  register holding it).
- Installation order: graphics init calls into the device — D3D11 `0x1413c4f50` (slot 0x58, writes
  +0xb1c, then `0x140b6b5b0`, slot 0x10), D3D12 `0x14139b300` (0x58, 0x10), Vulkan `0x14136cc30`
  (0x1a8 instance extensions), `0x141367cf0` (0x1b0 device extensions, 0x10), `0x141373fd0` (0x1b8,
  0x1c0 swapchain). All are skipped when the global is null, so **the device must be in the global
  before the gfx backend initialises**, or OpenXR-required Vulkan extensions and the device handles
  are never handed over.

### Base fields SS4 accesses directly (must exist in our object)

Fusion ctor `0x1fe06a0` initial values in the last column; offsets are identical in Fusion consumers
(`lab/device_calls_fusion.json` fields: 0x8, 0xc, 0x18, 0x1c, 0xb18, 0xb1c, 0xb3c, 0xb40, 0xb44).

| off | type | written by | read by (SS4) | meaning | init |
|---|---|---|---|---|---|
| +0x08 | int | engine `0x1401dcbe0` (=1 when not rendering the immersive game view), cleared by base `0x140b6a8e0` | `0x1401dcbe0`, getter `0x140b6af60` | frame drawn as 2D/cinema screen in VR | 0 |
| +0x0c | int | engine `0x1401dcbe0` (=1 on cinema path), `0x1401d96a0` (=0) | `0x1406db190` | cinema screen used this frame | 0 |
| +0x10 / +0x14 | int / float | base `0x140b6ae50(dev, float)` sets 1 / value; `0x140b6a8e0` clears | none in SS4 consumers (device-side) | heading set request from `CPSViewGrabber::EndViewGrabber` 0x1405ecca0 and `0x1405edd33` | 0 |
| +0x18 | int | engine renderer `0x1401dc520` (1, 2, 3, restore) | ~50 sites; passed as eye arg to 0x70/0x78; `==3` tests at `0x1403947c0`, `0x140720910` | eye currently being rendered | 0 |
| +0x1c | int | engine `0x1401dc520` (1 during VR render, 0 after), `0x1407bf460` (save/0/restore) | `0x140b6b3a0`, `0x140b6b3d0`, `0x140b6b400`, `0x140b6b420`, `0x14048dad0` | VR stereo render in progress | 0 |
| +0x20+r·0x30 (r=0..2) | int, +0x18 i64 | device (Fusion base #11 idle logic, button helper `0x1fdf500` stamps +0x38+r·0x30) | base `0x140b6ad20` (controller idle test, `vr_bEnableControllerIdleDetection`, `vr_fVRControllerIdleTime`) | per-role "moving" flag and last-activity time; set the int non-zero to never report idle | 0 / now |
| +0xb18 | int | device | engine game loop `0x140058800`: if ≠0 clears it and quits ("Requested by VR API") | quit request | 0 |
| +0xb1c | int | D3D11 init `0x1413c4f50` (1/2/3/4 from stereo-output objects) | `0x1401dc520`, `0x140ac9440`, `0x140b6b3d0`, `0x140b6b5b0` (→ `vr_iUsingSinglePassStereo`: 0→`vr_bSinglePrepareRendering`, 1/4→2, 2/3→4) | non-HMD stereo output mode; must stay 0 for an HMD | 0 |
| +0xb3c | int | device (Fusion `0x1fdf500` sets 1 whenever a controller value ≥ threshold) | `0x1400528a0`, `0x140058800`, `0x14040da90` | "VR controllers in use": forces the player's input profile to `"VRController"` | 0 |
| +0xb40 | int | engine `0x140058800` (0 or 2 from `0x140aa78a0` / `0x1400a8bd0`) | none in SS4 consumers | game state hint to the device; meaning unknown | 0 |
| +0xb44 | int | nobody in SS4 consumers (device) | `0x1401deaf0`, `0x1401dc520`, `0x14048dad0` | render only this eye (others cleared); also disables single-pass | 0 |
| +0xb68 | ptr | device | base `0x140b6ac00`, `0x140b6ab20`, `0x140b6ae30` (vrCalibrateOmniMovementDevice 0x140438f20) | Virtuix Omni object; **must be NULL** | 0 |

Base non-virtual helpers SS4 calls with the device in RCX (they only use the slots/fields above):
`0x140b6aca0` eye RT size = slot 0x60 × `vr_fRenderingPixelRatio`, or width override
`vr_pixRenderTargetWidthOverride` keeping aspect; `0x140b6ac70` slot 0x70 with
`ren_fNearClipPlaneDistance`/`ren_fFarClipPlaneDistance`; `0x140b6a8f0` culling frustum enclosing
eyes 1 and 2 (two slot 0x70 calls); `0x140b6ade0` button 7 pressed on either hand (slot 0xd0,
`(f0|f1)&5==1`); `0x140b6ad20` idle test; `0x140b6ae50`/`0x140b6a8e0` heading request set/clear;
`0x140b6af80` hidden-area stencil mesh from slot 0x178 (globals `0x1422aad98/9c/a0`).

Other globals tied to the device: `0x1422aa9a4` active gfx API (written 4 by D3D11 `0x1413c4e8e`,
6 by Vulkan `0x141367111`, 8 by D3D12 `0x14139ae7f`), `0x1422aa9b0` engine `CGfx*` passed to slots
0x28/0x18/0x40, `0x1422aacc4` explicit-timing flag gating slot 0x1c8 (SS4 only ever writes 0 at
`0x1400536a6`; Fusion's analogue `0x41f2d00` is set to 1 by CSteamVRDevice Init after
`SetExplicitTimingMode` — leave 0).

## Frame sequence (who calls what, in order)

Simulation tick, game loop `0x140058800`: slot **0x50**; then field +0xb18 check, +0xb40 write,
`0x140b6a8e0` (clears +0x8/+0x10).

VR view `0x1401dcbe0` (same structure as Fusion `0xf21e10`):
1. **0x30** BeginFrame/poll (Fusion: WaitGetPoses unless `vr_iSteamVRWaitGetPosesCallTime` picks another phase).
2. **0x1b8** HasDesktopOutput → if 0 the desktop drawport is dropped.
3. **0x120**(drawport) mirror window size.
4. Per eye (`0x1401d9e40`, `0x1401dabf0`, `0x1401dc520`): +0x18 = eye; **0x28**(gfx, eye) render target;
   **0x78** eye transform; **0x80** head placement; **0x70** projection; render into the returned target.
   Cinema/menu path uses `0x1401d9e40` per eye; single-pass path (`vr_iUsingSinglePassStereo≥1`,
   +0xb44==0, +0xb1c==0) renders into the eye-1 target once (R2 must resolve the layout).
5. `0x1401db670(…,1/2)` (0x28, 0xd0, 0x80 — controller/laser overlays), `0x1401dabf0`.
6. **0x38** (Fusion phase 1: after render, before submit).
7. `t1=0x28(gfx,1)`, `t2=0x28(gfx,2)`; `gfx->vt[0x288](gfx, *t, mode)` with mode 2 if API 8, 3 if API 6, else 0.
8. **0x18**(1, gfx, t1), **0x18**(2, gfx, t2) — **eye submit**.
9. **0x20** (Fusion phase 2: after submit).
10. Desktop mirror of eye textures per `vr_iDisplayEyeOnScreen` (`0x14224cb1c`); mixed reality via eye 3.

Present `0x140b5ca90`: **0x40**(CGfx*) immediately before `gfx->vt[0x2c8]` (swap), **0x48**() after.
Fusion phases 1..4 = slots 0x38, 0x20, 0x40, 0x48; each runs WaitGetPoses if
`vr_iSteamVRWaitGetPosesCallTime` equals the phase and PostPresentHandoff if
`vr_iSteamVRPostPresentHandoffCallTime` equals it (Fusion `0x41f2d0c`, `0x396cb00`).

## Slot table

Abbreviations: `P7` placement (7 floats), `M34`, `M44`, `CStr` CString, `S` = Fusion CSteamVRDevice,
`E` = CEmulatedVRDevice, `B` = CVRDevice base (when S/E inherit it). OpenVR methods are named from
their IVRSystem_019 / IVRCompositor_022 / IVROverlay_019 vtable offsets used in the bodies (e.g.
`sys+0xe0(dev,0,1000,…)` = GetStringTrackedDeviceProperty Prop_TrackingSystemName).

| SS4 off | F# | name / semantics | MSVC signature as called by SS4 | Fusion S / E behaviour | SS4 callers (live) | OpenXR implementation (safe default) | conf |
|---|---|---|---|---|---|---|---|
| 0x000 | 0,1 | scalar-deleting destructor | `void* dtor(uint flags)` | S `0x24e45a0`: resets compositor fade/grid (`comp+0x60/0x70/0x30`), `VR_Shutdown`, frees eye RTs +0xc10/+0xc18/+0xc38, strings; B `0x1fddad0` frees Omni +0xb68, idle data, clears `0x41f2d00` | `0x140b6af00` (shutdown, flags=0, engine frees memory after) | destroy XR session/swapchains; free nothing when flags==0 | H |
| 0x008 | 2 | Init | `int Init()` → 1 ok | S `0x24e5f60`: VR_Init, IVRSystem/IVRCompositor/IVROverlay, props 1000/1001/1005 → name/model/vendor strings +0xb88/90/98, recommended size → +0xba0, final RT size → +0xbb0, explicit timing for API 6/8; E: returns 1 | none (factory stripped) | call it ourselves; return 1 | H |
| 0x010 | 3 | SetGraphicsDevice | `void SetGfx(void* native)` | B `0x1fdd530`: store at +0xb50 | D3D11 `0x1413c4f50` (`0x1423bd400`, ID3D11Device*); D3D12 `0x14139b300` (`0x1423b7060`, ID3D12Device*: `vt[0x68]` = CheckFeatureSupport); Vulkan `0x141367cf0` (ptr to 4 qwords `{0x1423aff20, 0x1423afaf0, 0x1423afae0 (VkInstance: first arg of vkDestroySurfaceKHR before it), 0x1423afaf8 (VkPhysicalDevice: also passed to 0x1b0 and vkGetPhysicalDeviceSurfaceSupportKHR)}`) | keep handles for xrCreateSession graphics binding; 0x1423aff20/0x1423afaf0 not identified (VkQueue / VkDevice is the likely pair — verify) | H (meaning) / M (Vulkan field order) |
| 0x018 | 4 | **SubmitEye** | `void Submit(int eye, CGfx* gfx, Tex* tex)` (EDX, R8, R9) | S `0x24e6f50`→`0x24e6d90`: ignores `tex`, uses native handle saved at RT creation (+0xc20 eye1, +0xc28 eye2); `IVRCompositor::Submit(eye!=1, {handle, type=table[api-4] (4→DX11, 6→Vulkan, 8→DX12), gamma}, bounds{0, vMin, 1, vMax}, 0)`; v-bounds crop the shared vertical FOV to each eye (from GetProjectionRaw); gfx `vt[0x150]` lock around it; E: no-op | `0x1401dcbe0` (after `vt[0x288]` transition) | copy/blit `tex` (or our own target) into the eye's swapchain image (or render directly into swapchain images handed out by 0x28); release image; end frame after eye 2 or in 0x20/0x40 | H |
| 0x020 | 5 | FramePhase2 (after submit) | `void Phase2()` | S `0x24e94c0`: WaitGetPoses if WaitGetPosesCallTime==2; PostPresentHandoff if PostPresentHandoffCallTime==2; E no-op | `0x1401dcbe0` | good place for xrEndFrame (both eyes submitted) | H |
| 0x028 | 6 | **GetEyeRenderTarget** | `Tex* GetEyeRT(CGfx* gfx, int eye)` (RDX, R8D); NULL = none | S `0x24e51d0`: eye1/2 → `0x24e47d0`: (re)create RT of size `0x1fdf1c0()` (slot 0x60 × pixel ratio) via engine `0x1fb28a0(gfx,w,h,0,2,0,0)` (gfx `vt[0x98]`), store native handle (`gfx vt[0x1c0]`) for Submit, return `rt+8`; eye3 → RT of mixed-reality size +0xba8/+0xbac; else 0. E `0x24e2810`: eye1 always, eye2 only if `0x41f2cf4` | 25 functions incl. `0x1401d9e40`, `0x1401dc520`, `0x1401dcbe0`, `0x140ac9440`, `0x14062d810`, `0x1403db890` | return an engine texture object for eyes 1/2 sized from 0x60×ratio (engine RT creation in SS4 not yet pinned: `0x1401dc520` uses `0x140c6ca30`→`0x140e172f0` and `0x140c6c900`→`0x140e17030`, result `+8`, same shape as Fusion); NULL for eye 3 and 0 | H (semantics) / M (SS4 RT creator) |
| 0x030 | 7 | **BeginFrame / poll** | `void BeginFrame()` | S `0x24e9550`: WaitGetPoses (`0x24e6f70`: poses, head +0xbb8.., Euler +0xbf0, controller roles +0xc40+r·0xa0, buttons, device table +0xe40) unless the cvar selects phase 1..4; PostPresentHandoff unless phase 1..4/−1; haptics: per role `sys+0xc0` (axis types) then `sys+0x120` TriggerHapticPulse(dev, axis, +0xcd4) rate-limited by `vr_fMinTimeBetweenHapticPulses`, gated `vr_bEnableVibration`. E `0x24e3630`: keyboard/mouse emulated head and 3 controllers | `0x1401dcbe0` first call | xrWaitFrame + xrBeginFrame, xrLocateViews/spaces, xrSyncActions, update cached head/controller/button state, apply haptics | H |
| 0x038 | 8 | FramePhase1 (after render, before submit) | `void Phase1()` | S `0x24e96c0` (phase 1) | `0x1401dcbe0` | no-op | H |
| 0x040 | 9 | FramePhase3 (before swap) | `void Phase3(CGfx*)` (RDX=gfx in SS4; Fusion ignores it) | S `0x24e9750` (phase 3) | present `0x140b5ca90` (0x140b5cdf7) | no-op (or xrEndFrame if submit is deferred) | H |
| 0x048 | 10 | FramePhase4 (after swap) | `void Phase4()` | S `0x24e97e0` (phase 4) | present `0x140b5ce7c` | no-op | H |
| 0x050 | 11 | SimulationStep | `void Step()` | B `0x1fe1310` (not overridden): controller-idle detection (`vr_fVRIdle*`, `vr_bRecordDataForIdleRotation`), Omni update, then `vt[0x158]` SetPlaySpaceRotation(`vr_fPlaySpaceRotation`·π/180) | game loop `0x140058800` | apply `vr_fPlaySpaceRotation` (SS4 `0x1422aace8`) to our play-space; idle logic optional | H |
| 0x058 | 12 | GetAdapter | `int GetAdapter(int api, uint64* luid)` (EDX, R8) | S `0x24e3670`: api 4/8 → `sys+0x38` GetDXGIOutputInfo index, `*luid=0`; else −1. E −1 | D3D11 `0x1413c4f50` (api 4), D3D12 `0x14139b300` (api 8): ≥0 = adapter index; <0 and `*luid≠0` → match `DXGI_ADAPTER_DESC.AdapterLuid` | return −1 and `*luid` = LUID from xrGet{D3D11,D3D12}GraphicsRequirementsKHR | H |
| 0x060 | 13 | GetRecommendedRenderTargetSize | `PIX2D* Get(PIX2D* ret)` (ints w,h) | S `0x24e36b0`: +0xba0 (`sys+0x0`). E: +0xb70 (window size from 0x120) | `0x140b6aca0` (×pixel ratio → eye RT size, used by `0x1401dad19`, `0x1401dc520`), Vulkan `0x141376ee0` | `recommendedImageRectWidth/Height` of view 0 | H |
| 0x068 | 14 | GetRenderTargetSize (max) | `PIX2D* Get(PIX2D* ret)` | S `0x24e36c0`: +0xbb0 = max(0x60×ratio, two unnamed minimums `0x396cb58/54`, mixed-reality size). E/B `0x1fddd80` = 0x60 × ratio or width override | `0x140c88a10` (raises `gfx_pixMaxRenderTargetSize` budget) | same value as the eye RT actually created | H |
| 0x070 | 15 | **GetProjection** | `M44* Proj(M44* ret, int eye, float near (XMM3), float far ([rsp+0x20]))` | S `0x24e48b0`: eye1/2: `sys+0x10` GetProjectionRaw for both eyes; l,r from the eye, **t,b shared** = max(OpenVR bottom), min(OpenVR top) (OpenVR raw y is flipped: bottom=tanUp, top=−tanDown), frustum(l·n, r·n, t·n, b·n, n, f); eye3: `vr_fMixedRealityCameraVerticalFOV` + RT aspect; else identity. E `0x24e28d0`: symmetric from cvar FOV, horizontal shift | 18 functions (`0x1401d9e40`, `0x1400d7be0`, `0x1400ea890`, `0x1400ff760`, `0x140478a80` via `0x140b6ac70`, `0x140b6a8f0`…) | l=n·tan(angleLeft_eye), r=n·tan(angleRight_eye), t=n·tan(max angleUp of both eyes), b=n·tan(min angleDown of both eyes); report the same union FOV in the projection layer | H |
| 0x078 | 16 | **GetEyeTransform** (head→eye) | `M34* Eye(M34* ret, int eye)` | S `0x24e5060`: eye1/2: `sys+0x20` GetEyeToHeadTransform, translation ÷ worldScale, then **inverted** (`0x22d2000` is a 3x4 inverse); other eyes: mixed-reality camera matrix +0x2e50. E `0x24e24c0`: ±IPD/2 (`0x41f2cf0`) on x, identity rotation | 25 functions (`0x1401d9e40` composes it with head placement into the eye view matrix) | inverse of the eye pose located in VIEW space, translation ÷ `vr_fWorldScale`; identity for eye 0/3 | H |
| 0x080 | 17 | **GetHeadPlacement** | `P7* Head(P7* ret)` | S `0x24e36d0`: playspace quat(+0xb20)·head quat(+0xbb8), pos = rotate(playspace, head pos)+playspace pos(+0xb30), ÷ worldScale. E: +0xb94.. | 19 functions (`0x14071dc20`, `0x1401d9e40`, `0x1401dcbe0`, `0x1402a9070`, `0x140472e30`, `0x1408216b0`…) | VIEW pose in STAGE/LOCAL_FLOOR space (y = height above floor; OpenVR standing space convention), play-space rotation applied, ÷ worldScale | H |
| 0x088 | 18 | SetHeadPlacement | `void SetHead(const P7*)` | S `0x24e39a0` / E: store, y += 1.6 | `0x140440250` (injects bot/replay state: also 0xc8, 0xd8) | store into the cached head pose (override until next poll) | H |
| 0x090 | 19 | GetHeadPitch | `float Pitch()` | S `0x24e3a10`: +0xbf4 (Euler from head matrix, `0x22d3640`); E 0 | `CPSViewGrabber::EndViewGrabber` 0x1405ecca0 (builds {heading, pitch, 0}) | head pitch in engine angle units (unit not pinned; Fusion stores `0x22d3640` output) — 0 is safe | H (role) / M (unit) |
| 0x098 | 20 | GetHeadBanking | `float Banking()` | S `0x24e3a20`: +0xbf8; E 0 | none found | 0 | M |
| 0x0a0 | 21 | GetHeadHeadingDelta | `float HeadingDelta()` | S `0x24e3a30`: heading − previous heading (+0xbf0 − +0xbfc); E 0 | `0x1405ed710` (adds to view-grabber heading, then `0x140b6ae50`) | heading change since last poll; 0 safe | H |
| 0x0a8 | 22 | OnForcedControllerChanged | `void ResetRoleAssignment()` | S `0x24e4b50`: clear role serial strings +0xc48/+0xce8/+0xd88; E no-op | `vrOnForcedControllerChanged` 0x140b6a540 (tail jump) | re-evaluate hand assignment; no-op ok | H |
| 0x0b0 | 23 | GetRoleSerial | `const char* RoleSerial(int role)` (never NULL) | S `0x24e3a50`: CString at +0xc48+role·0xa0; E "" | `0x140762860` (camera calibration, role 2) | "" (static empty string) | H |
| 0x0b8 | 24 | **GetControllerPlacement** | `BOOL CtrlPlacement(int role, P7* out)` | S `0x24e3a70`: if role valid: playspace·pose(+0x10 of role entry) ÷ worldScale → 1; else identity → 0. E: +0xbb0+role·100 → 1. B: identity → 0 | `0x14043ac30`, `0x14043b920`, `0x140443fa0`, `0x14062cee0`, `0x140762860` | grip/aim pose of the hand in the same space as 0x80; 0 + identity when not tracked | H |
| 0x0c0 | 25 | GetControllerPlacement (second copy) | `BOOL CtrlPlacement2(int role, P7* out)` | S `0x24e3d70`: same transform from role entry +0x2c (WaitGetPoses writes the same pose to +0x10 and +0x2c; 0xc8 writes both); E → slot 0xb8 | `0x14043cf40` | return the same as 0xb8 | M (difference between the two copies unknown) |
| 0x0c8 | 26 | SetControllerPlacement | `void SetCtrlPlacement(int role, const P7*)` | S `0x24e4070`: role valid=1, both copies; E store | `0x140440250` | store into cached role pose | H |
| 0x0d0 | 27 | **GetControllerButton** | `float Button(int role, int button, uint* flags)` (EDX, R8D, R9) | S `0x24e4100`: value = role entry +0x4c+b·4, `*flags` = +0x70+b·4; E/B equivalent | 18 functions (`CVRCameraEntity::IsTrigger*`, `0x14094c1c0` input device, `0x14043ac30` VRCommandPressed, `0x1401dec10`, base `0x140b6ade0`…) | value 0..1 per engine button index; flags bit0 = pressed this frame, bit1 = held, bit2 = consumed, bit3 = released this frame (Fusion `0x1fdf500`; threshold 0.5, `vr_fTriggerThreshold` for button 0) | H (API) / M (button index map) |
| 0x0d8 | 28 | SetControllerButton | `void SetButton(int role, int button, float value (XMM3))` | S `0x24e4d20`: flags via `0x1fdf500` (edge detection, sets +0xb3c=1 and activity time), store value | `0x140440880` (loops buttons 0..9 for roles 0,1) | run the same edge logic into our button table | H |
| 0x0e0 | 29 | ConsumeButton | `void Consume(int button)` | S `0x24e4ff0` / E: OR 4 into flags of all three roles; button 7 also consumes 5 | `0x140156880`, `0x1401d8430` (buttons 0..9, and 7), `0x14040da90`, `0x14043dcb0`, `0x140aac120` | same | H |
| 0x0e8 | 30 | Recenter | `void Recenter()` | S `0x24e4140`: `sys+0x60` ResetSeatedZeroPose; E no-op | none found | recenter our play-space (or no-op) | H (Fusion) |
| 0x0f0 | 31 | PrintInfo | `void PrintInfo()` | S `0x24e4e40`: console "VR device: Name/Model/Vendor/Render target size"; E "Using emulated VR device." | `0x140b99130` (stats dump) | print runtime/system name and RT size | H |
| 0x0f8 | 32 | EnumerateControllerSerials | `void Serials(CStaticArray<CString>* out)` | S `0x24e5b90`: serials of devices with flags&5==5; E "Not supported" | `0x140761450`, `0x140762860` (mixed-reality camera calibration) | leave empty | H |
| 0x100 | 33 | IdentifyController | `void Identify(const char* serial)` (RDX = string data) | S `0x24e5d60`: queue identify for device with that serial, else "Couldn't find controller with serial number %1"; E "Not supported" | `CMSGSCameraCalibration::CameraController_OnChange` 0x14075fb20, `…IdentifyCameraController_OnClick` 0x14075fc50, `0x140761380` (all with `vr_strForceAux0ControllerSN`) | no-op | H |
| 0x108 | 34 | AssignControllerRoles | `void AssignRoles()` | S `0x24e9c10`: left/right/aux from model names and `vr_strForce{Right,Left,Aux0}ControllerSN`; B `0x1fdda10` clears duplicate forced SNs | `0x140762860` | no-op (OpenXR gives hands directly) | H |
| 0x110 | 35 | **GetDeviceName** | `CStr* Name(CStr* ret)` | S `0x24e4db0`: "Rift" if manufacturer contains "Oculus", "WindowsMR" if contains it, else "Vive"; E "Emulated" | `vrGetDeviceName` 0x140b6a560 and 23 functions comparing with "Emulated", "WindowsMR", "Knuckles" (`0x1401d8430`, `CPlayerVRData::IsTouchpadPressed*`, `AreControllersSwapped` 0x140438af0, `0x14044a6c0`, `0x14044e880`, `0x1408ee4b0`…) | "Vive" (default SteamVR controller logic) | H |
| 0x118 | 36 | GetControllerTypeName | `CStr* CtrlName(CStr* ret, int hand)` (R8D = 0 right / 1 left) | S `0x24e5740`: prop 7000 (ControllerType) of the left-hand device: "knuckles" → "Knuckles", else slot 0x110 (Fusion takes no hand arg) | `0x140444a20`, `0x14044a780`, `0x14062f8b0`, `0x140720a30`, `0x1407219e0`, `0x14079d380` (compare "Vive", "Knuckles", "WindowsMR") | "Knuckles" for Index controllers (enables 0x210 finger path), else "Vive" | H |
| 0x120 | 37 | SetMirrorWindow | `void SetMirror(CDrawPort*)` (R15 drawport) | S/B no-op; E `0x24e2730`: window size = box at +0x10..+0x1c | `0x1401dcbe0` | store size; no-op ok | H |
| 0x128 | 38 | IsAppFocused | `BOOL Focused()` | S `0x24e41e0`: !IVROverlay::IsDashboardVisible (1 without overlay); E 1 | none found | session state FOCUSED | M |
| 0x130 | 39 | **UpdateVibrations** | `void Vibrations(CStaticArray<VibEntry>* list)` (entry 0x20 bytes: +0 int loop, +4 strength, +8 float time, +0x10 effect ptr (curves at +0x10/+0x18, duration at +8), +0x18 int right, +0x1c int left) | S `0x24e52f0`: sums effects into pulse length 0..3500 µs for role 0 (+0xcd4) and role 1 (+0xd74), emitted in 0x30; E no-op | `0x1412ee6a0` (`vsl_fGlobalVibrationStrength`) | amplitude = sum/3500 per hand → xrApplyHapticFeedback (short duration) in 0x30 | H |
| 0x138 | 40 | StopVibration | `void StopVib(int role)` | S `0x24e4190`: zero role pulse; E no-op | `0x1412ee5e0`, `0x1412ee6a0`, `0x1412eeb19` (roles 1 and 0) | xrStopHapticFeedback | H |
| 0x140 | 41 | IsDashboardVisible | `BOOL Dashboard()` | S `0x24e41b0`: `overlay+0x210`; E 0 | `0x140761380`, `0x1407622b0`, `0x140762860` | 1 when session not FOCUSED | H |
| 0x148 | 42 | **GetDeviceAPI** | `int API()` | S 1, E 4 (Oculus device = 8); strings in `vrGetDeviceAPI`: 1 "SteamVR", 4 "Emulated", 8 "Oculus" | `vrGetDeviceAPI` 0x140b6a5c0, `0x140050860` (branches on ≠4, together with `vr_bVRControllerExclusiveInput`), `0x140442f80`/`0x1404431e0` and `0x14044d6c0` (==1 SteamVR branches) | 1 | H |
| 0x150 | 43 | SetPlaySpaceRotation | `void SetPlaySpace(float rad (XMM1))` | B `0x1fdd950`: +0xb20 = quat(heading), +0xb30 = origin | none found in SS4 (Fusion: from base 0x50) | store our play-space yaw | H (Fusion) |
| 0x158 | 44 | IsKeyboardShown | `BOOL KbShown()` | S +0x2e40; E 0 | none found | 0 | H (Fusion) |
| 0x160 | 45 | ShowKeyboard | `BOOL ShowKb(CStr type, BOOL multiline, …)` | S `0x24e4ee0`: IVROverlay::ShowKeyboard ("Password"/"Submit" mode); E 0 | none found | 0 | M |
| 0x168 | 46 | HideKeyboard | `void HideKb()` | S `0x24e5030`: `overlay+0x258`; E no-op | `0x14090ebf0` | no-op | H |
| 0x170 | 47 | SetKeyboardListener | `void SetKbListener(void*)` | S: stores to global `0x43c1b60`; E no-op | none found | no-op | M |
| 0x178 | 48 | **GetHiddenAreaMesh** | `const float2* Mesh(int eye, int* triCount)` (EDX, R8) | S `0x24e4230`: `sys+0x108` GetHiddenAreaMesh(eye!=1, standard): `*triCount`, returns vertex pointer (UV 0..1); E 0/NULL | `0x140b6af80` (needs both meshes, uses min count, packs L.x/R.x per vertex → `vr_bMaskHiddenArea` stencil; else "[VR] Cannot obtain hidden area mesh!") | NULL + count 0 (XR_KHR_visibility_mask meshes are not index-paired between eyes, so the engine's packing would be wrong) | H |
| 0x180 | 49 | GetTrackedDevicesCount | `int Count()` | S 64; E 0 | `CPlayerVRData::GetTrackedDevicesCount` 0x140436f10 | 0 (or 3: head + hands) | H |
| 0x188 | 50 | GetTrackedDeviceFlags | `uint Flags(int idx)` | S `0x24e9e60`: table +0xe40+idx·0x80 from `0x24e5860`: 1 connected, 2 pose valid, 4 controller, 8 generic tracker, 0x10 left role, 0x20 right role | `CPlayerVRData::GetTrackedDeviceFlags` 0x140436f80 | 0 | H |
| 0x190 | 51 | GetTrackedDevicePlacement | `P7* DevPlacement(P7* ret, int idx)` | S `0x24e9e80`: matrix +0xe44+idx·0x80 → placement (`0x22d5560`); E identity | `0x14043d460` | identity | H |
| 0x198 | 52 | GetTrackedDeviceSerial | `CStr* Serial(CStr* ret, int idx)` | S `0x24e4fb0`: +0xe78+idx·0x80 (prop 1002); E "" | `CPlayerVRData::GetDeviceSerialNumber` 0x140437070 | "" | H |
| 0x1a0 | 53 | GetPressedControlsForDevice | `uint Pressed(int idx)` | S `0x24e4270`: bits for values >0.5 of roles whose device index == idx; E 0 | `CPlayerVRData::GetPressedControlsForDevice` 0x140437100 | 0 | H |
| 0x1a8 | 54 | GetVulkanInstanceExtensions | `CStr* VkInstExt(CStr* ret)` | S `0x24e4b90`: `comp+0x138`, space-separated; E "" | Vulkan `0x14136cc30` (splits on ' ', enables each) | xrGetVulkanInstanceExtensionsKHR string | H |
| 0x1b0 | 55 | GetVulkanDeviceExtensions | `CStr* VkDevExt(CStr* ret, VkPhysicalDevice)` (R8) | S `0x24e4c50`: `comp+0x140`; E "" | Vulkan `0x141367cf0` | xrGetVulkanDeviceExtensionsKHR string | H |
| 0x1b8 | 56 | HasDesktopOutput | `BOOL Desktop()` | B 1 | `0x1401dcbe0` (0 → no drawport), Vulkan `0x141373fd0` (0 → no window surface; image count from 0x1c0), `0x141374ea0` (0 → skip present) | 1 | H |
| 0x1c0 | 57 | GetOffscreenImageCount | `int ImageCount()` | B 0 | Vulkan `0x141373fd0` (only when 0x1b8 returned 0) | 0 | M |
| 0x1c8 | 58 | SubmitExplicitTimingData | `void ExplicitTiming()` | S `0x24e4150`: `comp+0x150`; E no-op | Vulkan `0x141388880`, D3D12 `0x1413a89b0` (only if `0x1422aacc4`≠0) | no-op (keep `0x1422aacc4`=0) | H |
| 0x1d0 | 59 | InitSkeletalInput | `void InitSkeleton(const CStr* manifest)` | S `0x24e5600`: unless controller type contains "knuckles", creates IVRInput_006 helper (0x88 bytes) at +0x2e48 with "/actions/main", skeletonl/r; E no-op | `0x1400517e0` | no-op | H (call) / M (argument meaning) |
| 0x1d8 | 60 | HasSkeletalInput | `BOOL HasSkeleton()` | S `0x24e4170`: +0x2e48 != 0; E 0 | `0x140442750`, `0x140444a20`, leaf `0x14043aac0` | 0 (1 only with finger curls in 0x1e0) | H |
| 0x1e0 | 61 | GetFingerCurl | `float Curl(int finger 0..4, BOOL handFlag)` (EDX, R8B = `hand==1`) | S `0x24e4d80`→`0x24eae70`: value from skeleton helper, −1.0 if none; E −1.0 | `0x1407228c0` (`vr_fSkeletonFingerPoseFactor`) | XR_EXT_hand_tracking / grip value; −1.0 safe | H |
| 0x1e8 | 62 | (skeleton helper present) | `BOOL ()` | S `0x24e4180`: +0x2e48 != 0; E 0 | none found (Fusion caller `0x1aca5d0`) | 0 | M |
| 0x1f0 | — | ShowSteamVRActions | `void ShowActions()` | — | `vrShowSteamVRActions` 0x1404390a0 | no-op | H (call) |
| 0x1f8 | — | IsEyeTracking | `BOOL EyeTracking()` | — | `vrIsEyeTracking` 0x140b6a7a0 | 0 (1 with XR_EXT_eye_gaze_interaction) | H (call) |
| 0x200 | — | GetEyeTrackingData | `void EyeData(EyeRec out[2])`; EyeRec = M34 (0x30) + float at +0x30 (ctor `0x140f43ab0`: identity, 1.0) | — | `vrGetEyeTrackingData` 0x140b6a7f0 (converts each M34 to a placement via `0x141146cd0`, pushes the float) | leave records untouched | H (layout) / L (meaning of the float) |
| 0x208 | — | GetGloveTracker | `void* Glove()` | — | `0x14043e330` (wraps it into `CPlayerVRData+0x6d0`, read by `CPlayerVRData::GetGloveTracker` 0x140438ea0) | NULL | H (call) / M (meaning) |
| 0x210 | — | HasHandSkeleton | `BOOL HandSkeleton(int hand)` | — | `0x140444a20`, `0x14044a780`, `0x14044d6c0` (only checked when 0x118 returned "Knuckles") | 0 | H (call) / M (meaning) |

No SS4 call site uses an offset above 0x210 on this object; `device_slots.py` hits above 0x218 were
other objects (the stricter tracker drops them). Whether the stripped SS4 class had more slots after
0x210 is not determinable; fill a few extra entries with a logging stub that returns 0.

### Fusion role-entry layouts (private to the device; SS4 never reads them)

CSteamVRDevice role entries: 3 × 0xa0 at +0xc40 (+0 valid, +8 serial CString, +0x10 P7, +0x2c P7,
+0x48 device index, +0x4c 9 float values, +0x70 9 flag words, +0x94 haptic µs, +0x98 last pulse time).
CEmulatedVRDevice: 3 × 100 bytes at +0xbb0 (P7, values at +0xbcc, flags at +0xbf0). SS4 feeds
10 buttons per role (`0x140440880`, `0x1401d8430` loop 0..9), so our table needs ≥ 10 entries.
Button meaning in Fusion depends on controller type (`0x24e6f70`): index 0 trigger (grip if
`vr_bSteamVRSwapGripTrigger`), 2/3 trackpad-or-stick x/y; 7 is the button SS4 treats as menu/back
(`0x140b6ade0`, consumes 5 with it). The remaining index map is still to be derived from the
`CVRInputDevice` control names (`ETRSVRDeviceControlName.*`).

## Minimal OpenXR device (what must be real for first light)

Live every frame: 0x30, 0x28, 0x18, 0x20, 0x38, 0x40, 0x48, 0x50, 0x60, 0x68, 0x70, 0x78, 0x80,
0xb8, 0xd0, 0x130, 0x1b8. At gfx init: 0x10, 0x58, 0x1a8, 0x1b0. Returning engine CStrings: 0x110,
0x118, 0x198, 0x1a8, 0x1b0 (engine ctor `0x1410bfac0`). Everything else can return the defaults in the
table. Fields to initialise: all zero except +0x20/+0x50/+0x80 (non-zero = controllers never idle) and
+0xb3c (set to 1 once VR controller input is active).

## Open items / blockers

1. **Engine render-target creation in SS4** (slot 0x28): Fusion creates eye RTs through
   `0x1fb28a0` (gfx vt+0x98) and takes the native handle via gfx vt+0x1c0. The SS4 equivalents are
   not pinned; candidates are `0x140c6ca30`/`0x140e172f0` (create) and `0x140c6c900`/`0x140e17030`
   (get, result `+8`) as used by `0x1401dc520`. Needed before 0x28/0x18 can be implemented
   (or 0x28 must return an engine object wrapping our swapchain image) — R5 scope.
2. Native texture handle from the engine `Tex*` given to 0x18 (SS4 gfx vtable offset unknown; the
   engine calls `gfx->vt[0x288](gfx, *tex, mode)` on it just before submit).
3. Vulkan 0x10 payload: fields 0 and 1 (`0x1423aff20`, `0x1423afaf0`) not identified; D3D12 needs the
   command queue for `XrGraphicsBindingD3D12KHR`, which slot 0x10 does not provide.
4. Single-pass stereo layout of the eye-1 target (`0x1401dc520` with `vr_iUsingSinglePassStereo≥1`):
   R2.
5. Installation timing: R1_device_seam.md plans to store the pointer "after engine init"; it has to be
   before the gfx backend initialises (see Object lifetime), or those init-time slots never run.
6. Unit of 0x90/0xa0 (Fusion `0x22d3640` Euler output) not confirmed; returning 0 is safe.
7. SamSM.exe (global `0x142287ed0`): offsets of fields and slots not yet re-verified; run
   `tools/device_calls.py` on it with `docs/re/builds/SamSM.json`.
