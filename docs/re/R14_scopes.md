# Gate R14: picture-in-picture scopes (Sam4.exe 616154, SamSM.exe 628492)

Static analysis only (Ghidra copy `lab/ghidra_r14`, decompiles `lab/r14/{a..i}.c`; raw disassembly and the
reflection tools for SamSM; nothing was run). Addresses are Sam4.exe PE VAs unless marked SM. "player" is
`CPlayerActorPuppetEntity`, "vrd" is `CPlayerVRData` (player+0x25c8, SM +0x25d0, R3), "weapon" is the weapon
entity, "mode" is a `CWeaponMode` object, "device" is our `COpenXRDevice` in `[0x1422aad10]` (SM `0x142287ed0`),
"gfx" is the engine `CGfx*` in `[0x1422aa9b0]`. Member names/offsets come from reflection records
(`meta_class.py`, `meta_members.py`, `meta_offset.py`); content facts from class and file names in the CTSEMETA
files (extracted to `lab/r14/s4`, `lab/r14/sm`, never committed). Member *values* inside .ep/.rsc were not decoded
(no value reader exists), so per-weapon numbers (zoom factor, FOV multipliers) are unknown statically.

New read-only tool: `tools/port_check.py <src.exe> <dst.exe> <src>:<dst>[=name] …` compares the instruction shape
of a candidate SamSM port (mnemonic-sequence ratio and the list of differing struct displacements, first .pdata
fragment). It confirms ports found by call order or by a delta from a neighbour when `port_list.py` reports
"not found" because a player field moved by 8 in SM.

## 0. Summary

* **Scoped weapons.** All player weapons are `CGenericWeaponParams` → `CGenericWeaponEntity`. Real scopes
  (magnifying, with the black mask overlay) are the **secondary weapon mode `CSnipingWeaponMode`**: SS4 *Sniper*
  (`Sniper_Scope.rsc`), *Devastator* (`Devastator_Scope.rsc`); SM adds the *Crossbow* (`Crossbow_Scope.rsc`).
  Aim-down-sights without overlay is `CAimableWeaponMode`: SS4 Assault Rifle (`AssaultRifle_Sight.rsc`, holo sight),
  Pistol (`Pistol_Aim.rsc`); SM adds the AK (`AK_Sight.rsc`). The secondary mode is `en_pwmSecondaryWeaponMode`
  **weapon+0x320** (both builds) and is driven by **`plcmdAltFire`** ("Aim/Secondary Fire"; virtual pad
  LeftTrigger).
* **Zoom = FOV multiplier on the player.** Both modes write `en_fWeaponFOVMultiplier` (player **+0xfcc**, SM +0xfd4)
  through player `vt+0x1230` = 0x1404946c0 (SM 0x140494db0). The desktop projection multiplies the player FOV by
  it (0x140477e60). **In VR the world projection never uses it**: 0x140478a80 returns device slot 0x070 whenever
  device+0x18 ≠ 0. So the world eyes already keep the HMD FOV; only side effects remain (look-speed scaling,
  single-pass HUD projection, §2.5).
* **The black overlay is native and VR-gated.** The mask model (`Sniper_HUD.mdl`/`Crossbow_HUD.mdl`, drawn by
  0x1407c1a30) and the 2D zoom HUD (0x1407c0eb0) both return immediately when `en_bOwnerPlayingInVR`
  (weapon+0x1f0) ≠ 0. The overlay the user saw means weapon+0x1f0 was 0 at that time (no local vrd, or the weapon
  did not pick up the flag). With the local vrd active (R3 §10) it should not be drawn; first runtime check (§7).
* **The engine already has a VR picture-in-picture scope**, from the Fusion-era `CSniperWeaponEntity` (still
  compiled in, not used by any SS4/SM content): weapon `vt+0x640` 0x1407bf9c0 renders the world into a
  render-target holder sized `swp_vScopeCanvasSize` from the model attachment `"Crosshair"` with
  `swp_fScopeFOVDeg`, then binds it as the model texture parameter `"Scope"`. It is the reference recipe.
* **Extra camera through the device: eye 3.** `vr_eye_render` 0x1401dc520 renders a third full view after both
  eyes when `vr_iMixedRealityOutput` ≠ 0: device+0x18 = 3, target = device slot 0x028(gfx, 3), projection = slot
  0x070(3), view = eye-3 head ∘ slot 0x078(3). Every input is a device slot, so the mod can render a scope camera
  once per frame with no engine patch, by raising the cvar only between its own slots 0x030 and 0x038.
* **Recommendation (§6):** native zoom state untouched; scope camera = barrel placement, FOV = native desktop
  scoped FOV; render through eye 3 (once per frame, only while the sniping mode is aiming); present as an OpenXR
  quad layer at the eyepiece with a circular alpha mask; right or left hand from `en_bRightHandWeapon`; purely
  local, multiplayer-neutral.

## 1. Content inventory (who has a scope)

| weapon (.ep) | game | secondary mode (.rsc) | mode params class | mask model | aimed rendering params |
|---|---|---|---|---|---|
| `SniperWeapon.ep` (+`_DM`) | SS4, SM | `GenericWeaponModes/Sniper_Scope.rsc` | `CSnipingWeaponModeParams` | `Models/Weapons/Sniper/Sniper_HUD.mdl` | `SniperWeaponRenderingAimed.ep` |
| `DevastatorWeapon.ep` | SS4, SM | `GenericWeaponModes/Devastator_Scope.rsc` | `CSnipingWeaponModeParams` | `Sniper_HUD.mdl` (same mask) | `DevastatorWeaponRenderingAimed.ep` |
| `CrossbowWeapon.ep` | SM | `SeriousSamSM/…/WeaponsModes/Crossbow_Scope.rsc` | `CSnipingWeaponModeParams` | `Models/Weapons/Crossbow/Crossbow_HUD.mdl` | `CrossbowWeaponRenderingAimed.ep` |
| `AssaultRifleWeapon.ep` | SS4, SM | `AssaultRifle_Sight.rsc` | `CAimableWeaponModeParams` | none | `AssaultRifleWeaponRenderingScopeAimed.ep` (holo sight `Sight.mdl`, `Sight_Glass_Mat`, `RedDot.tex`) |
| `PistolWeapon.ep` | SS4, SM | `Pistol_Aim.rsc` | `CAimableWeaponModeParams` | none | `PistolWeaponRenderingAimed.ep` |
| `AKWeapon.ep` | SM | `WeaponsModes/AK_Sight.rsc` | `CAimableWeaponModeParams` | none | `AKWeaponRenderingAimed.ep` |

Evidence: `meta_types.py` over all four .gro archives finds `CSnipingWeaponModeParams`/`CSniperZoomResources`
only in the three `*_Scope.rsc` files (SS4: Sniper, Devastator; SM: those plus Crossbow), and no
`CSniperWeaponParams`/`CSniperZoomHandler`/`CAutoShotgunWeaponParams` object anywhere; the DTTY of every player
weapon .ep is `CGenericWeaponParams` (exceptions: knife, hands, sticky bomb, typing, melee items). Mask textures:
`Textures/Interface/SniperMask{,_NM,_Refraction}.tex` (+ SM `CrossbowScope_{Lines,Round}.tex`).

Not scopes: the legacy `CSniperWeaponEntity`/`CAutoShotgunWeaponEntity` (code only), `CPSTelescope`
(`EnterTelescopeState` 0x140423450; byte scan of every .wld/.lua/.ep/.rsc/.scp/.nfo/.xml/.txt in both games finds
it only in editor help `Help/Macros/GameTemplate/Project_macros.xml`), `plcmdZoom`/`PPF_ZOOMING` (§2.4).

## 2. Native zoom

### 2.1 `CSnipingWeaponMode` (scope)
Vtable **0x141c85f20** (SM **0x141c6a790**, slot-for-slot match), size 0x98 (factory `vt+0x258` 0x140a73080
allocates 0x98). Members: `swm_bFirePressed` +0x20, `swm_szhZoomHandler` +0x28 (embedded `CSniperZoomHandler`,
0x60), `swm_bCurrentlyAiming` +0x88, `swm_fAimRatio` +0x8c, `swm_pssEnterSnipeMode` +0x90 (sound). Mode+8 = weapon,
mode+0x10 = mode params (getter 0x140090e60). Params `CSnipingWeaponModeParams`: `swmp_szrZoomResources` +0xa8,
**`swmp_fZoomFactor` +0xd8**, `swmp_fAimedSpeedMultiplier` +0xdc, `swmp_bSuspendZoomDuringRecoil` +0xe0,
`swmp_fAimTime` +0xe4, `swmp_pgrAimFOVChange` +0xe8, `swmp_pwrpWeaponRenderingParamsAimed` +0xf0,
`swmp_idEnterSnipeSoundTerm` +0xf8. All offsets identical in SM.

| slot | Sam4 / SM | behaviour |
|---|---|---|
| +0x40 press | 0x140a73330 / 0x140a738a0 | `+0x20 = 1`; `+0x88` = 1 (hold) or toggled, by profile option `0x140478950(player)+0xa8` |
| +0x48 release | 0x140a73390 / 0x140a73900 | `+0x20 = 0`; in hold mode also `+0x88 = 0` |
| +0x30 activate | 0x140a73990 / 0x140a73f00 | loads zoom resources (0x1407c0bf0, params+0xa8); handler +0x10 = 0 (full-zoom time), **+0x14 = +0x18 = 1/`swmp_fZoomFactor`** |
| +0x20 step | 0x140a73a20 / 0x140a73f90 | aim ratio +0x8c ramps by dt/`swmp_fAimTime` up (aiming) or down; plays the enter-snipe sound once; at ratio 1 and aiming → handler on (0x1407c0a10), else handler off (0x1407c0a80); weapon+0x348 = !fullyZoomed (`en_bDiscreteFirePower`); handler step 0x1407c0d60 |
| +0x18 deactivate | 0x140a738c0 / 0x140a73e30 | handler off, drop sound |
| +0x60 shoot placement | 0x140a73570 / 0x140a73ae0 | only if weapon+0x1f0 == 0 and zoomed: shots from the view (desktop); **VR keeps the barrel** (R8) |
| +0x180 anim event | 0x140a73920 / 0x140a73e90 | recoil event + `swmp_bSuspendZoomDuringRecoil` → handler off |
| +0x198 speed | 0x140a734a0 / 0x140a73a10 | `swmp_fAimedSpeedMultiplier` while zoomed |
| +0x1b8 aim ratio | 0x140a733e0 / 0x140a73950 | +0x8c through graph `swmp_pgrAimFOVChange` |
| +0xc0..+0xd8 | 0x140a73510, 0x140a73600, 0x140a73720, 0x140a73420 | desktop view-model blend toward the aimed rendering params |
| +0xe8 render (pass 3) | 0x140a73c30 / 0x140a741a0 | zoomed and pass 3 → **mask overlay 0x1407c1a30** |
| +0xf0 HUD | 0x140a73be0 / 0x140a74150 | → **2D zoom HUD 0x1407c0eb0** |
| +0x240/+0x268 | 0x140a73100, 0x140a73c90 | save/restore (+0x88, +0x8c, handler) |

The caller that issues `vt+0xe8` with pass 3 was not traced (unknown).

### 2.2 `CSniperZoomHandler` (shared with the legacy sniper)
Members: `szh_bZoomActive` +0, `szh_tmStartZooming` +8, `szh_fFullZoomingTime` +0x10, `szh_fMinFOVMultiplier`
+0x14, `szh_fMaxFOVMultiplier` +0x18, `szh_fZoomRatio` +0x1c, `szh_pssZooming` +0x20, `szh_bLastFrameZooming` +0x28,
textures wheel/eye/arrow/led +0x30..+0x48, `szh_ulSniperZoomTextFlags` +0x50, `szh_pmiSniperMask` +0x58 (same in SM).
In the mode it sits at mode+0x28, so the active flag is **mode+0x28**, the ratio mode+0x44.

* On 0x1407c0a10 (SM 0x1407bffa0): active = 1; `player vt+0x1230(max + (min−max)·ratio)` unless
  0x14047edb0(player) (the bot/spectator predicate).
* Off 0x1407c0a80 (SM 0x1407c0010): active = 0; `player vt+0x1230(1.0)`.
* Step 0x1407c0d60 (SM 0x1407c02f0): while active and fire pressed, ratio = elapsed/full time, same FOV write;
  handles the zooming sound.
* For a sniping mode min = max = 1/`swmp_fZoomFactor`, so **zoomed FOV = desktop FOV / zoom factor**, instantly.

### 2.3 `CAimableWeaponMode` (ADS)
Vtable 0x141c770e8 (SM 0x141c5b958), step `vt+0x20` **0x140a604e0** (SM 0x140a60930). `wm_fAimRatio` +0x20 ramps
with `wmp_fAimDurationFP` +0xa8 (first person) / `wmp_fAimInDurationTP`/`OutDurationTP` +0xac/+0xb0; FOV write
`player vt+0x1230(1 + (wmp_fFOVMultiplier(+0xb4) − 1)·graph(ratio))`. No overlay. Whether a given sight magnifies
(`wmp_fFOVMultiplier` < 1) is a content value (unknown statically; read params+0xb4 at runtime).

### 2.4 Input
* Scope: `plcmdAltFire` handle 0x142285a30 ("TTRS:PlayerCommand.AimSecondaryFire", `CommandTable.lua`;
  `DefaultVirtualGamepadBindings.lua`: `LeftTrigger`; `DefaultVRInputControllerBindings.lua`: `ButtonA`). Readers:
  controller 0x14040d68a/0x14040d75d fragments and 0x140414a8c. The fire bits travel in `ClientAction`
  (`ubFireButtonStates`, R3 §4), so the mode press/release and the zoom state run natively on every machine that
  simulates the weapon. The path from the fire bit to mode `vt+0x40/+0x48` was not traced (M).
* Dual wield: with two weapons, AltFire fires the left weapon (bindings + `*_DW` rendering params exist for the
  sniper); a scope is therefore only reachable single-wielded (inferred, M).
* `plcmdZoom` (0x142285820, SM 0x142262970) / `plcmdZoomToggle` (0x142285a38, SM 0x142262b88): alive input
  0x1404135c0 sets/clears `PPF_ZOOMING` (0x10000, enum table 0x141858c20) in `en_ulPlayerPuppetFlags` +0xfa8 (SM
  +0xfb0) at 0x14041394c (SM 0x140413827); player step 0x140486a40 ramps `en_fZoomFactor` +0xfd0 (SM +0xfd8) by
  `ppp_fZoomingSpeed` (+0x94c) at 0x140488611/0x140488636 (SM 0x140488bae/0x140488bd3). **Neither command is
  registered** by `CommandTable.lua` and no default binding uses them, so this generic zoom is unused in practice.

### 2.5 FOV and VR
* Desktop FOV: `FOV = clamp(0x140477aa0(player), 5°, 135°) × 0x140477e60(player)` in 0x140478a80 (SM
  0x1404790b0, base 0x1404780d0, factor 0x140478490). Factor = `((1 − zoom)(1 − ppp_fZoomingMinFOVRatio) +
  ppp_fZoomingMinFOVRatio) × en_fWeaponFOVMultiplier` (player+0xfd0, params+0x948, player+0xfcc), unless a
  script override from 0x140ba6bd0 is ≥ 0. Base FOV = puppet params +0x90, `plr_fFOVOverride` 0x142248850,
  vehicle/ride overrides.
* **VR:** 0x140478a80 calls device slot 0x070 for eye `device+0x18` whenever device+0x18 ≠ 0, and the stereo
  branch of the render view 0x14048dad0 uses slot 0x070/0x078 (two-pass) or 0x140b6a8f0 (single-pass). The zoom
  factor never reaches the world projection: **world eyes keep the HMD FOV natively**.
* Side effects that remain in VR while scoped:
  1. **Look speed**: alive input 0x1404135c0 multiplies the heading/pitch deltas by 0x140477e60 (calls at
     0x140413fb5/0x140413fd3, SM 0x140413e82/0x140413ea0). Pad turning slows by the zoom factor while scoped.
  2. **Single-pass HUD**: with device+0x18 == 0, 0x140478a80 falls back to the zoomed desktop FOV, so the
     crosshair element (R8 §B) projects with the wrong FOV while scoped.
  3. The render view stores `FOV × factor` in a per-player view record (0x14004ec30 result +0x4c); its consumer
     (LOD/streaming is the likely one) is unknown.
  4. 0x14040b060 (camera-follow springs, `vrd == 0` only) — no effect with a vrd.

### 2.6 The overlay and how it is suppressed
* Mask: 0x1407c1a30 (SM 0x1407c0fc0): `if (weapon+0x1f0 == 0 && gfx->drawport)`: ortho projection fitted to the
  drawport aspect (0x141111ac0), draws `szh_pmiSniperMask` (0x140c209e0), then clears the side bars black (gfx
  `vt+0x268`). This is the full-screen black overlay with the circle.
* 2D HUD: 0x1407c0eb0 (SM 0x1407c0440): `if (weapon+0x1f0 == 0 && active && drawport)`: distance text
  (player+0x2194, colour from the hit entity), zoom text `1/FOVmul`, wheel/eye/arrow/led textures.
* `en_bOwnerPlayingInVR` weapon+0x1f0 is written `= (owner+0x25c8 ≠ 0)` at 0x1402d4e20 (SM 0x1402d4a80),
  0x1402ddb7b (fn 0x1402dda60) and 0x1402e1690. **With the local vrd present both overlays are skipped by the
  engine; no suppression hook is needed.** If the overlay still shows in VR, weapon+0x1f0 is 0 for that weapon
  (runtime check 1); do not force the flag, it also selects the VR barrel and fire placement.

### 2.7 What stays native
Fire, damage (fire modes, R8 §3), spread, sounds (`swm_pssEnterSnipeMode`, `szh_pssZooming`), aimed speed
multiplier, recoil zoom suspension, aim-ratio state and its replication all live in the mode/handler/player and
are not touched by any of the recommendations below.

## 3. Rendering an extra camera

### 3.1 Eye 3 (the mixed-reality third view)
In `vr_eye_render` 0x1401dc520 (SM 0x1401dc0f0), after the two-pass loop **and** after the single-pass branch
(both reach LAB_1401dc9ba):

```
0x1401dc9ba  if (vr_iMixedRealityOutput[0x1422d40c0] != 0 && vr_iRenderMixedRealityVideo[0x1422d40e8] == 0
                 && viewer+0x124 == 0) {                 // SM gate 0x1401dc59c, cvars 0x1422b1280 / 0x1422b12a8
               saved = device+0x18; device+0x18 = 3;
               rt = device->vt[0x28](gfx, 3);
               if (rt) { render state push 0x1401c2da0(…, 3|4, 1); gfx vt+0x258(rt); clear black (vt+0x268);
                         gfx vt+0x258(0); 0x1401deaf0(pi, 0, rt)  // = pi->vt[0x50](pi, rt, 1): a full Render
                         optional gfx vt+0x308(2, 0, rt) }
               gfx vt+0x310(); device+0x18 = saved; }
```
* `viewer+0x124` is 1 only around 0x1401dd800 (the VR background-simulation render, 0x1401dae65..0x1401dae77);
  0 in normal frames.
* Inside that Render, every camera input is a device slot: projection = 0x140478a80 → **slot 0x070(eye 3)**
  (near/far `ren_fNear/FarClipPlaneDistance` 0x14224d388/0x14224d38c); view = player camera
  (`vt+0xdf8` → 0x1404794a0 → head world 0x14043cb50) **∘ slot 0x078(3)** (0x14048dad0, same code as eyes 1/2);
  HUD and crosshair use slot 0x070/0x078 of the current eye too (0x1400d7be0).
* Eye-3 differences: head world 0x14043cb50 (SM 0x14043cc90) and anchor 0x140443c50 use the **MR anchor
  0x14043af00** (SM 0x14043b0a0) instead of vrd+0x2c0 when device+0x18 == 3. From the decompile it is the anchor
  rotated by −`vrd+0x674` (the stick-turn yaw) about the current local head position (M), i.e. the eye-3 rig does
  not follow artificial turning. Controller placement for eye 3 is the second copy 0x14043cf40 (slot 0x0c0, MR
  anchor). Objects with flag 0x400 are skipped (0x1403947c0; `SkipInMixedRealityCameraView` scripts), weapons
  are skipped when `vr_bSkipMixedRealityWeaponRendering` 0x1422d40fc (SM 0x1422b12bc) is set (readers
  0x1402e03de laser, 0x1407cc706, 0x140ccc171).
* Other readers of `vr_iMixedRealityOutput` that run in a frame: the render view re-places the vrd laser
  pointers with 0x14043cf40 (0x14048f50f, SM 0x14048fb16), 0x1401a8320 (entity registration with
  0x1403958e0, meaning unknown), and the **desktop composite** in the VR view after submit (0x1401dd510, SM
  0x1401dd18b: `if (cvar) { 0x1401dec60(slot0x28(3)); 0x1401de760(…) ; skip the normal mirror }`).
  Frame order in 0x1401dcbe0: slot 0x030 → … → 0x1401dc520 (eyes, then eye 3) → 0x1401db670 ×2 → 0x1401dabf0 →
  **slot 0x038** → slot 0x028(1/2) → submit 0x018 → slot 0x020 → composite gate. So a device that sets the cvar
  in slot 0x030 and clears it in slot 0x038 gets exactly one eye-3 render and no desktop composite.
* Fusion reference (R1): slot 0x028(3) returns an RT of the mixed-reality size (+0xba8/+0xbac), slot 0x070(3)
  uses `vr_fMixedRealityCameraVerticalFOV` and the RT aspect, slot 0x078(3) returns the MR camera matrix
  (+0x2e50), and slot 0x068 includes the MR size in the max RT size.

### 3.2 The engine's own VR scope (legacy `CSniperWeaponEntity`, Fusion design)
Vtable 0x141ad3db0. Called per Render from the player render (0x14048fad0 fragment: left weapon `vt+0x640` at
0x14048fe12, right at 0x14048fe2e; SM 0x140490412/0x14049042e), before the main view; the generic weapons have the
no-op stub there (0x140044530, shared — never patch it).
* `vt+0x640` = **0x1407bf9c0** (SM 0x1407bef40): if weapon+0x1f0, device present and the model has a
  `"Crosshair"` attachment (0x1407bebd0): holder at weapon+0x420 (alloc 0x90, 0x140c6c6b0), `holder_create`
  0x140c6ca30(holder, gfx, `swp_vScopeCanvasSize` params+0x35c/+0x360, fmt 5, 0, 2, 60.0f; the last two
  arguments' meaning is unknown); set target
  0x140b58e40; clear (gfx vt+0x268); **render 0x1407bf460**; unset 0x140b585f0 (SM 0x140b58b00); resolve
  (gfx vt+0x288 or 0x140b5b080); bind as the model-instance texture parameter **`"Scope"`** (0x1400b91d0, SM
  0x1400b9360, on `0x140c11210(weapon+0x90)`).
* 0x1407bf460 (SM 0x1407be9e0): saves device+0x1c and sets it to 0; camera = 0x1407bdd80 (SM 0x1407bd300) =
  weapon placement (vt+0x9b8) ∘ attachment `"Crosshair"` (0x1407be510 → 0x140c29660) ∘ vt+0x750; view
  0x1411467c0 into gfx +0xc70…; projection 0x141111460(`swp_fScopeFOVDeg` params+0x364, canvas aspect, near, far)
  into gfx +0xdf0…; 0x140b603d0; viewer entity = player (0x140ca3280); `vr_bMaskHiddenArea` = 0,
  `ren_iRescalingFilter` = 1; world render core **0x140bc1cd0(world) → 0x140bc14a0 → 0x140bc1610(0) →
  0x140bc0d20(0, gfx, 1) → 0x140bc0970** (SM 0x140bc23c0, 0x140bc1b90, 0x140bc1d00, 0x140bc1410, 0x140bc1060);
  restore. No HUD is drawn.
* `vt+…` 0x1407bff10 (SM 0x1407bf4a0): `prj_bGameOptionCloseOtherEyeWhenSniping` 0x1422739dc (SM 0x142250ac0):
  picks the eye (0x14043c7b0 eye world, SM 0x14043c8f0) within `swp_fHandToHeadAimThreshold` (+0x358) of a hand
  and fades the other eye to black over `swp_fVREyeFadeTime` (+0x368) (0x1407bf280 → fade 0x140055c50).
* Fusion VR has the same members (`swp_fScopeFOVDeg`, `swp_vScopeCanvasSize`, `swp_fVREyeFadeTime`,
  `swp_fHandToHeadAimThreshold`) and readers of the close-eye option at 0x13dbab0/0x13ddfe0 (not decompiled).
* The generic `CSnipingWeaponMode` has **no** such path: SS4 replaced the VR scope with nothing (the overlay is
  just skipped in VR).

### 3.3 Other render-to-texture paths
* 0x1403db890 (device slot 0x028 caller): low-resolution world render (eye-1 RT size or window/16) with several
  cvars zeroed, same render core with 0x140bc0d20(0, x, 1). Purpose unknown (probe/exposure).
* Cinema 0x1401d96a0: `pi->vt[0x50](pi, tex, 1)` into viewer+0x30 (R10).
* Mirrors (`ren_bMirrors`, `CMirrorProperties`) and `CRemoteCameraViewerEntity::SetScreen` 0x1406e3170 exist;
  not traced (they are in-world features, not an arbitrary-camera API).

### 3.4 Thread and cost
* Everything above runs on the main loop thread inside Render (R10 §5.7); device slots 0x028/0x070/0x078 for
  eye 3 are called on that thread while device+0x18 == 3.
* Eye 3 is a complete `Render(pi, rt, 1)`: visibility, shadows, effects, post-processing and the HUD are
  recomputed exactly like one more eye in two-pass mode (R2/EYE_TARGETS: no sharing between passes is known).
  Whether shadow maps are reused across views is unknown. GPU cost scales with the scope RT (e.g. 768² vs a
  ~2.5k² eye); CPU cost (culling, draw submission) is roughly one more view, reduced by the narrow scope frustum.
* The legacy path (3.2) uses the same world core without HUD; its cost is the same order. In Fusion it ran once
  per Render, i.e. per eye; a mod version must gate it to once per frame.

## 4. Scope geometry on the models

* Identifier (IDNT) tables, i.e. bone/attachment/child names, kind not decoded:
  * `Sniper.mdl` (child of `Sniper_Arms.mdl` at `R_Weapon`/`L_Weapon`): `Weapon_Root, Magazine, Bolt, Barrel,
    Trigger, Barrel01, Muzzle, ClipBullets, LeftHandGripEffector, Fire_TP, Fire`. **No scope/lens/eyepiece
    identifier**; the scope is part of `Sniper.bmf` (materials `Sniper_0_Sniper_01_Mat`, `_02_Mat`, no lens
    material).
  * `Devastator.mdl`: `… Muzzle, Barrel01, LeftHandGripEffector, Scope, Bullets`; child `Scope_02.mdl` (mesh
    material named `Scope`, standard `ARM_ColorNormal.nsh`).
  * SM `Crossbow_FP.mdl`: bones `ScopeCap_01`, `ScopeCap_02`, `ScopeCapRelease`, `Barrel01`, `Fire`, `Fire02`
    (arms rig included); mesh material `Scope`.
  * `AssaultRifle.mdl`: `Sight`, `FoldedSights` children (`Sight.mdl`: `Sight_Glass_Mat`, `RedDot.tex`).
* No material in these meshes has a texture parameter named `"Scope"` (they use the standard roughness-metallic
  presets), so the legacy "bind the RT as the lens texture" trick has no slot to bind to without new content.
* VR model params exist (`wp_pmcVRModel` +0xd8, `wp_pctLeftHandToolVR` +0xf0, `wp_pctRightHandToolVR` +0xf8,
  `wp_vVRHandsOffsetFromHead` +0x148, `wp_eTwoHandedVROrentation` +0x1bc), but no scoped weapon's .ep references a
  `*_VR.mdl` (its string table lists only `*_Arms(_L).mdl` and `PlayerWeapon_*_L/R.ctl`), so the VR placement uses
  the normal model.
* Native queries:
  * `GetBarrelPlacement` **0x1402d7220**(weapon, P7* out, 0x1411223e0() (meaning unknown, passed by the script
    wrapper), CName attachment, 0, 0, 1) (call shape from script wrapper 0x1402d1ab0; SM 0x1402d6e40). Empty name → `weapon vt+0xa70` default shooting
    attachment. With weapon+0x1f0 it reads the hand-placed model (0x1402d7360 → **0x140c29660** attachment/bone
    lookup, SM 0x140c29c50) and applies 0x1402d6910. Any identifier above (`Muzzle`, `Barrel01`, `Scope`) can be
    asked for; whether bones (vs attachments) resolve through 0x140c29660 is unknown (the bone-matrix query is
    0x140c2bb90, R9).
  * Hand placement `vrd_controller_world` 0x14043b920(vrd, out, role) (SM 0x14043ba20); eye world
    0x14043c7b0(vrd, out, eye) (SM 0x14043c8f0); hand of a weapon = `en_bRightHandWeapon` weapon+0x118.
* Eyepiece position relative to `Weapon_Root`/`Barrel01` is not in the data we can read (mesh not parsed): it
  must be a per-weapon constant measured in the lab (or from `ScopeCap_01/02` on the crossbow).

## 5. SamSM 628492 ports

| item | Sam4 | SamSM | how |
|---|---|---|---|
| `CSnipingWeaponMode` vtable | 0x141c85f20 | 0x141c6a790 | `vtable_slot of` the ported +0xe8/+0x60 slots; all 79 slots align |
| `CAimableWeaponMode` vtable / step | 0x141c770e8 / 0x140a604e0 | 0x141c5b958 / 0x140a60930 | sig + vtable |
| handler on / off / step | 0x1407c0a10 / 0x1407c0a80 / 0x1407c0d60 | 0x1407bffa0 / 0x1407c0010 / 0x1407c02f0 | sig, `port_check` 1.000 |
| mask / 2D HUD | 0x1407c1a30 / 0x1407c0eb0 | 0x1407c0fc0 / 0x1407c0440 | sig, `port_check` 1.000 |
| set FOV multiplier (player vt+0x1230) | 0x1404946c0 (+0xfcc) | 0x140494db0 (+0xfd4) | SM player vtables 0x141831500/0x141852a50 slot +0x1230 |
| FOV base / factor / projection | 0x140477aa0 / 0x140477e60 / 0x140478a80 | 0x1404780d0 / 0x140478490 / 0x1404790b0 | `port_check` 1.000 (factor: 0xfd0→0xfd8, 0xfcc→0xfd4) |
| head world / MR anchor / ctrl copy eye 3 | 0x14043cb50 / 0x14043af00 / 0x14043cf40 | 0x14043cc90 / 0x14043b0a0 / 0x14043d080 | port_check; MR anchor = call target in SM head world (0x14043ccfe) |
| `vr_eye_render` / eye-3 gate | 0x1401dc520 / 0x1401dc9ba | 0x1401dc0f0 / 0x1401dc59c | EYE_TARGETS, `cvar_readers` |
| desktop MR composite gate | 0x1401dd510 | 0x1401dd18b | `cvar_readers` |
| legacy scope RT / view / camera / close-eye | 0x1407bf9c0 / 0x1407bf460 / 0x1407bdd80 / 0x1407bff10 | 0x1407bef40 / 0x1407be9e0 / 0x1407bd300 / 0x1407bf4a0 | sig, port_check |
| world render core | 0x140bc1cd0, 0x140bc14a0, 0x140bc1610, 0x140bc0d20, 0x140bc0970 | 0x140bc23c0, 0x140bc1b90, 0x140bc1d00, 0x140bc1410, 0x140bc1060 | sig / constant delta + port_check |
| holder create / get, set RT / unset | 0x140c6ca30, 0x140c6c900, 0x140b58e40, 0x140b585f0 | 0x140c6cfb0, 0x140c6ce80, 0x140b59380, 0x140b58b00 | sig; unset from the SM legacy call sequence |
| weapon prerender calls (`vt+0x640`) | 0x14048fe12 / 0x14048fe2e | 0x140490412 / 0x14049042e | `vcall_sites` + `port_sites` |
| look-speed FOV scaling | 0x140413fb5 / 0x140413fd3 | 0x140413e82 / 0x140413ea0 | `port_sites` (calls the SM factor) |
| zoom ramp (player step) | 0x140488611 / 0x140488636 | 0x140488bae / 0x140488bd3 | `field_refs` +0xfd8 |
| `en_bOwnerPlayingInVR` writer | 0x1402d4e20 | 0x1402d4a80 | `port_sites` |
| cvars | MR output 0x1422d40c0, MR video 0x1422d40e8, skip weapons 0x1422d40fc, close eye 0x1422739dc | 0x1422b1280, 0x1422b12a8, 0x1422b12bc, 0x142250ac0 | harvest JSON |

Unchanged in SM: every mode/handler/params member offset above, weapon+0x1f0, +0x118, +0x320. Player fields
move by +8 (+0xfcc→+0xfd4, +0xfd0→+0xfd8, +0xfa8→+0xfb0). Unrestricted builds were not ported.

## 6. Recommended implementation

**(a) Zoom FOV for the scope camera only.** Leave the native state machine alone; the world eyes are already
unaffected (§2.5). Scope camera vertical FOV = the native desktop scoped FOV, computed exactly like the non-VR
branch of 0x140478a80: `clamp(0x140477aa0(player), 5, 135) × 0x140477e60(player)` (both pure readers; do not call
0x140478a80 from a device slot, it recurses into slot 0x070). That is the desktop FOV divided by
`swmp_fZoomFactor` for sniping modes and the ADS multiplier for aimable ones. Active condition: right or left
weapon (player+0xf90/+0xf94, SM +0xf98/+0xf9c) has `*(weapon+0x320)` with the sniping vtable and handler active
(mode+0x28) — or, for a smooth fade-in, `swm_fAimRatio` mode+0x8c > 0. Optional: undo the look-speed scaling
(§2.5.1) for VR pad turning only if headset tests show it is unwanted.

**(b) Scope camera placement.** Use the barrel placement the shots use: `0x1402d7220(weapon, &p, 0x1411223e0(),
empty, 0, 0, 1)` (empty = the engine empty literal 0x141567715 — SM literal not resolved here — which selects the shooting attachment via
`vt+0xa70`, 0x1402d7360; SM 0x1402d6e40). The reticle centre is then the point of impact, the
camera sits in front of the weapon mesh (no self-occlusion), and the 5–8 cm scope-to-bore offset is negligible at
sniping range. Sample it on the main thread once per frame before the eyes render (device slot 0x030), from
the normal (eye 1/2) placement, never during the eye-3 pass (eye 3 re-places weapons with the MR anchor, §3.1).
Store it as an offset relative to the hand (`inv(vrd_controller_world(role)) ∘ p`, role from weapon+0x118), so
the presentation (d) can reuse it in XR space.

**(c) Render path: device eye 3, once per frame, only while scoped.**
1. Slot 0x030: if a scope is active this frame, set `vr_iMixedRealityOutput` = 1 (saving the user value), set
   `vr_bSkipMixedRealityWeaponRendering` = 1, and keep `vr_iRenderMixedRealityVideo` = 0.
2. Slot 0x028(gfx, 3): return the scope RT (an engine holder like the eye targets, EYE_TARGETS; 512²–1024²,
   square). Return NULL whenever no scope is active, so nothing renders. Report the size in slot 0x068.
3. Slot 0x070(3): perspective from (a), aspect 1, `ren_fNear/FarClipPlaneDistance`.
4. Slot 0x078(3): the transform from the eye-3 head to the scope camera, in the same encoding as eyes 1/2 but
   **without** the ÷`vr_fWorldScale` (both ends are engine placements): `inverse(inv(H3) ∘ S)`, where
   `H3 = 0x14043cb50(vrd)` evaluated inside this slot (device+0x18 is 3 there, so it is the eye-3 head) and `S` is
   the world scope placement from (b) (hand offset ∘ current normal-anchor hand placement).
5. Slot 0x038: restore both cvars before the desktop-composite gate (0x1401dd510).
6. Check whether the HUD/crosshair is drawn into the eye-3 image (0x1400d7be0 has no eye-3 test that we found).
   If it is, skip it while device+0x18 == 3 with the smallest guard (a check in the HUD render entry), or accept a
   crosshair at the aim point as the reticle.
Cost: one extra full view at the scope resolution only while aiming; optionally every second frame (reuse the
last image) if the perf matrix needs it.
*Fallback:* the Fusion recipe (§3.2) from a once-per-frame point (first `vt+0x640` call of the frame via a
detour of the player render, not the shared stub), with explicit camera/projection and no HUD. It needs the CGfx
view/projection fields (+0xc70, +0xdf0, +0x16c0, +0x1050, +0x1c6c…) written by the mod, which are raw offsets
and need per-build verification first, so it is second choice.

**(d) Presentation: an OpenXR quad layer at the eyepiece.** Copy the eye-3 image (captured like the eye images)
into its own XR swapchain and submit an `XrCompositionLayerQuad` after the projection layer, posed at the
eyepiece: grip pose (XR space) ∘ per-weapon eyepiece offset (engine offset × `vr_fWorldScale`, plus
`vr_fWeaponScale`), facing back along the barrel, size ≈ eyepiece diameter (≈4 cm), circular alpha mask (and an
optional drawn reticle) applied in the mod's copy shader, `XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT`.
Replacing the lens texture is not possible without new content (no `"Scope"` texture slot in SS4/SM materials,
§4). The eyepiece offsets per weapon (Sniper, Devastator, Crossbow; optionally the AR holo sight) are lab-measured
constants. Optional comfort: fade the quad in by eye-to-eyepiece distance (0x14043c7b0) and, as in Fusion, dim the
other eye (`prj_bGameOptionCloseOtherEyeWhenSniping` semantics) as a mod option.

**(e) Hands and dual wield.** One scope at a time (only the single-wielded weapon reaches its secondary mode,
§2.4). Use the hand from `en_bRightHandWeapon` (+0x118) of the weapon whose secondary mode is the active sniping
mode, for both the camera offset (b) and the quad pose (d). For two-handed weapons the barrel placement already
includes the native two-hand blend (0x1402e2d70, R3).

**(f) Multiplayer neutrality.** Nothing is written to entities: the mode, handler, FOV multiplier and fire bits
stay native and replicate as they do now. The cvars are toggled locally within one frame and are not replicated
(client options). Remote VR players: their sniping mode runs on our machine too, but the active condition is
evaluated only for the local player, and eye 3 is rendered only from the local device. Desktop peers have no
device, so none of this runs there.

## 7. Unknowns and runtime checks

1. In VR with the local vrd, scope with the sniper: log weapon+0x1f0, `*(weapon+0x320)` vtable, mode+0x28,
   mode+0x8c and player+0xfcc. Expected: +0x1f0 = 1 and **no mask overlay**; FOV multiplier = 1/zoom factor.
2. HUD/crosshair in the eye-3 image (§6 c.6); whether 0x1401a8320's MR-only registration has visible effects
   while the cvar is raised for one frame.
3. The fire-bit → secondary-mode path and the dual-wield AltFire behaviour (§2.4).
4. Shadow/visibility reuse between views (cost of eye 3) — measure in the perf matrix.
5. Content values: `swmp_fZoomFactor`, `wmp_fFOVMultiplier` per weapon (read at runtime from params +0xd8/+0xb4).
6. Eyepiece offsets per weapon (no lens/eyepiece identifier in Sniper.mdl); whether `ScopeCap_01/02` resolve
   through 0x140c29660.
7. The caller of mode `vt+0xe8` pass 3 and of `vt+0xf0`; the consumer of the per-player view record FOV (§2.5.3).
8. The exact algebra of the MR anchor 0x14043af00 (derived as "anchor yaw removed about the head", M); the
   recommendation avoids depending on it by evaluating 0x14043cb50 during eye 3.
