# SS4 + Siberian Mayhem 6DoF VR mod — plan

## Context
The goal is a full 6DoF VR mod for Serious Sam 4 (build 616154) and Siberian Mayhem (build 628492). Both are 64-bit Windows exes of the same Serious Engine 4 branch (`Sam4_dev`), run under Proton.

Required features:
- dual wield
- physical melee with inertia and collision
- weapon wheel and gadget wheel, with gadgets held and used in hand
- maximum-performance single-pass stereo
- multiplayer with networked IK, with desktop players (who have the mod installed) able to join
- 2D menus projected into VR
- vehicles with immersive controls

Proton/Monado comes first (Bigscreen Beyond, Knuckles, RTX 4090); SteamVR is also supported, and the code must stay native-Windows compatible.

**What recon found:**
- Both exes still contain the Serious Sam Fusion VR gameplay and rendering layer:
  - 138 `vr_*` cvars: `vr_iUsingSinglePassStereo`, `vr_bSinglePrepareRendering`, `vr_bMaskHiddenArea`, `vr_fMinSwingToHitSpeed`, …
  - About 90 `CPlayerVRData::*` script functions, plus `ClientActionVR`, `CVRMeleeWeaponEntity`, `CVRWeaponSelectionWheelInfo`, `CPlayerVRLegMovementAnimations` and `CVRCameraEntity`.
  - Shader VR plumbing: `cbuffer CBVREye`, `GetVREye`, multiview UVs, left/right constant pairs.
  - Vulkan `VK_KHR_multiview` and `VK_KHR_fragment_shading_rate`.
  - Leftover VR assets: `*_VR.mdl`, `DefaultVRInputControllerBindings.lua`, Knuckles rendering .ep files.
- **Stripped:** the HMD device layer (`CVRDevice`, `CSteamVRDevice`, `CEmulatedVRDevice`, `CVRInputDevice`, `steamStartVRDevice`, `VR_InitInternal2`), VR locomotion (teleport and strafe), most `CVRWeaponSelection*` button classes, and `vr_bEnable`.
- Fusion 2017's native Linux VR ELF and its content (`/home/obesecatlord/FastGames/SteamLibrary/steamapps/common/Serious Sam Fusion 2017`) still have all of these (it works on this machine via XRizer) and is the reverse-engineering reference. The other references are TLH (`SamTLH.exe`, MSVC) and Talos VR.

**Core strategy:** bring the engine's own VR consumers back to life by installing an OpenXR-backed replacement VR device, and drive native gameplay state machines rather than reimplementing them. Shared code comes from the SS2 VR mod (MIT, header-only `src/common`, tested) and from eduke32_vr's designs.

**User decisions:**
- Proton first, Windows also supported.
- New repo at `~/Documents/ss4sm-vr`.
- Full-body IK, visible locally and networked.
- SS2 engineering rules apply: no invented offsets, refuse unknown builds by fingerprint, no duplicate gameplay state machines, no teleport. No external review gate.

## Architecture

1. **Loader.** Sam4.exe and SamSM.exe statically import `MSIMG32.dll`, and only `AlphaBlend` (verified with objdump).
   - Ship `Bin/x64/msimg32.dll`: it forwards the 5 exports to the system copy, loaded by full path, then loads `Bin/x64/ss4smvr/ss4smvr.dll`.
   - `DllMain` only registers `LdrRegisterDllNotification`; real init happens later on game threads.
   - Proton launch: `WINEDLLOVERRIDES="msimg32=n,b" PRESSURE_VESSEL_IMPORT_OPENXR_1_RUNTIMES=1 %command%`.
   - No game file is modified.
2. **In-process OpenXR.** The Khronos `openxr_loader.dll` sits in the payload directory. Under Proton it uses wineopenxr; on Windows it uses SteamVR, Monado or Oculus. Unlike SS2, no host process is needed because the game is 64-bit.
3. **Graphics: Vulkan with `XR_KHR_vulkan_enable2`.**
   - Hook vkGet*ProcAddr and send instance/device creation through `xrCreateVulkan{Instance,Device}KHR`.
   - Filter physical devices.
   - One queue lock covers submit, present and XR frame calls.
   - Force `gfx_strAPI` to Vulkan in memory before graphics init (cvar address from the registration tables); never write the user's config.
   - Why Vulkan: under Proton, the engine's D3D11 stereo paths need nvapi or AMD AGS, which DXVK doesn't provide.
   - Fallback (only if Gate R5 fails): D3D11 through DXVK with `XR_KHR_D3D11_enable`, already proven by SS2 on this machine.
4. **Engine seam: `COpenXRDevice`.** A hand-lowered MSVC-x64 vtable of `extern "C"` functions; no engine C++ classes are declared on our side.
   - Slot order rebuilt from three sources: Fusion's `CSteamVRDevice` vtable, TLH's MSVC bodies, and the SS4 call sites (`prjIsVRActive` → `[rax+0x10]`, the `CPlayerVRData::*` bodies).
   - Slots not yet identified get trap stubs that log their index once.
5. **Addresses.** Generated per build by `harvest_symbols.py`, which walks the engine's cvar, script-function and meta-class registration tables, plus signatures unique across all 4 exes (Sam4, Sam4_Unrestricted, SamSM, SamSM_Unrestricted).
   - Fingerprint: SHA-256, TimeDateStamp, SizeOfImage, `.text` CRC and build string.
   - An unknown build means inert payload and a flat game.
6. **Toolchain.** As in SS2: MinGW-w64 GCC with C++20, CMake, and MinHook plus OpenXR-SDK pinned via fetch.
   - ABI tests cover struct sizes and hook prologues.
   - No C++ exceptions or classes cross the engine boundary.
7. **Lua bridge.** Register C functions into the statically linked Lua 5.1.3 and run a world-bootstrap chunk from the DLL, so no `.gro` is needed for logic.
   - The stock `Network.lua` `CreateRPC` has an unreliable-flag bug, so call `CallScriptRPC(name,false,…)` directly.
8. **Licensing.** The new repo is MIT. SS2 `src/common` is ported as-is. eduke32_vr (GPL-2) is a design reference only; its standalone `eduke_xr` files could be relicensed by you if you want them copied directly.

## Repo layout (`~/Documents/ss4sm-vr`)
```
AGENTS.md README.md CMakeLists.txt cmake/mingw-w64-x86_64.cmake
docs/ IMPLEMENTATION_STATUS.md re/<gate>.md re/builds/<sha>.json USER_VR_TEST.md MIXED_DESKTOP_VR.md
src/proxy/   msimg32 forwarders + payload load
src/core/    engine-agnostic headers (ported SS2 src/common + new: melee_sim, body_ik, wire)
src/xr/      session FSM, actions, projection/quad/cylinder layers, visibility mask, haptics, perf
src/vk/      dispatch hooks, enable2 redirect, queue lock, stereo targets, multiview inject, mirror, timestamps
src/se4/     fingerprint, symbols(gen), hooks, cvars, lua_bridge, vr_device, camera, input, player,
             weapons, melee, gadgets, vehicles, ui_capture, body_ik_apply, net, server_mode   ← shared SS4+SM
src/games/{ss4,sm}/  build tables, vehicle/gadget catalogs, content roots                    ← per-game only
tools/  build.py package.py install.py (from SS2), fingerprint.py, harvest_symbols.py, sigscan.py, ghidra/,
        extract_lua.py (private lab dir, never committed), monado_{pose,input}_driver.cpp + runtime_lab.py (from SS2),
        mp_lab.py, perf_report.py, vk_xr_probe/
tests/  portable unit tests + symbol-table consistency over 4 exes
```

**Port from SS2:**
- Source: `/home/obesecatlord/LargeGames/Development/ss2-vr-development/src/common/`
  - `math`, `ui` (ComfortAnchor), `head_comfort`, `options_layout`, `controls`
  - `weapon_wheel` (WheelRing), `two_hand`, `weapon_alignment`, `muzzle`, `lasers`
  - `grenade_throw`, `bomb_fuse`, `steering_wheel`, `rider`, `ride_grasp`, `arm_ik`
  - `foveation`, `wear_calibration`, `physical_motion`, `hook_transaction`, `build_contract`
  - The wire/versioning parts of `network.hpp`
- Their matching tests.
- `tools/{build,package,install}.py` and the Monado lab drivers.

**Re-implement from eduke32_vr designs** (`/home/obesecatlord/Documents/eduke32_vr/eduke32-vr`):
- `source/vr/eduke_xr.cpp`: session loss/retry, reference-space change handling, 5 controller profiles plus hand tracking, GPU timer queries.
- `source/duke3d/src/vr.cpp`:
  - `solveIkBody`: 45° yaw deadzone, stride legs, two-bone pole vectors, arm stretch up to 1.6x.
  - Menu world-lock with yaw recenter (>55°, 750 ms).
  - Wrist-twist wheel selection.
- `vrshoot.h`: the `VRBodyPose` and `VRShotInput` designs.
- `tools/vr/software_xr_lab.py`: llvmpipe Monado.

## Phases (each is done only when SS4 **and** SM pass)

### Phase 0: Recon and RE gates (no gameplay code)
**Setup:**
- Launch each game flat once on Vulkan and D3D11 to create the prefixes; record config, log and save paths.
- Run `harvest_symbols.py` on all 4 exes and diff the results.
- Extract the Lua scripts and `Help/Cvars/*.xml` into a private lab directory.

**Gates:**
- **R1, device seam.** Find the device global pointer, the `vr_strAPI` factory and the slots used.
  - A: consumers and factory are intact → implement `COpenXRDevice`.
  - B: no factory → write our device pointer to the global after init.
  - C: consumers stripped → SS2-style hooks for each feature (about +2–3 months).
- **R2, stereo path.** Use Ghidra plus RenderDoc under Proton with forced cvars to map what the Vulkan backend does with `vr_iUsingSinglePassStereo` and `vr_bSinglePrepareRendering`, the multiview `viewMask` and `CBVREye`.
  - R2a: a native multiview/broadcast mode works → use it.
  - R2b: only single CPU prepare with two GPU passes → ship that, then do Phase 1b.
- **R3, live VR gameplay.** Classify each item as live, dormant-callable or dead:
  - `SetPlayingInVR`, `ClientActionVR`
  - `CVRMeleeWeaponEntity` and its HitScriptEvent
  - `CVRGrenadeWeaponEntity`
  - the `CVRWeaponSelection*` remnants
  - the VR leg animations and the `CIKStaticModelEntity` VR movement handler
  - rotation and movement cvars
  - native ray/hull cast and damage functions
- **R4, content mounting.** Does an add-only `zz_ss4smvr.gro` load in the normal exe, the Unrestricted exe, and via the workshop path? Does a multiplayer join check content? Never modify `All.dat` or official .gro files; Fusion broke with "Required signing key couldn't be found".
- **R5, graphics interop.** Run `vk_xr_probe.exe` in the game prefix on Monado and SteamVR. Check enable2, multiview and VRS passing through winevulkan, queue sharing, and the visibility mask.

**Exit:** each gate's `docs/re/<gate>.md` records its outcome and the per-build addresses.

### Phase 1: Vertical slice, head-tracked stereo in menu and level
**Work:**
- Proxy, payload, fingerprint refusal, logging.
- Vulkan redirect and XR frame loop:
  - `xrWaitFrame` paces the render thread.
  - The predicted display time drives head poses.
  - Native vsync off.
  - The desktop mirror is a one-eye crop blit.
- The device seam from R1, and stereo through the best R2 path.
- One CPU visibility pass with a union frustum and shared shadows.
- Hidden-area mask.
- Menu as a quad layer, initially from backbuffer capture.
- Performance HUD with GPU time per pass and CPU frame time.

**Phase 1b, only if R2b:**
- Hook DXC compilation so `GetVREye()` returns `SV_ViewID`.
- Inject `viewMask=0b11` and 2-layer targets into the geometry render passes.
- Run post-processing per eye on a double-wide image filled by a layer-to-half copy.
- Keep a mod-owned shader cache that never touches `CachedShaders_VLK.gro`.

**Verify:**
- Lab with a fixed Monado pose: the eyes show correct parallax.
- Simulation ticks over 60 s are identical with VR on and off.
- The visibility pass runs once per frame (counter).
- 90 Hz on the Beyond at the recommended resolution on a reference level.
- A patched exe is refused.

### Phase 2: Input, hands, locomotion
**Work:**
- OpenXR actions for Index, Touch, Vive, WMR and simple profiles.
- Feed the native "VRController" input device if it is live, so bindings stay rebindable natively. Otherwise inject at the player-action builder.
- Per-hand aim through `GetControllerPlacementLeft/Right` consumers or shooting-placement hooks (SS2 `muzzle.hpp` pattern).
- Locomotion: smooth, head- or hand-relative; smooth turn at 150°/s with a snap option.
- Roomscale: HMD translation becomes native movement, with a fade when collision pushes back.
- Recenter chord.
- Haptics: route `StartVibration*` and `.vfx` to the correct hand.
- Menu pointing: the hand ray drives the native cursor (SS2 `src/game/menu.cpp` approach). The cursor is drawn on the panel; there is no beam.

**Verify:** Monado input-driver replays; logged command edges; hand rays line up with hits on a test wall.

### Phase 3: Weapons
**Work:**
- **Dual wield:** native `plcmdToggleDualWielding`, per-hand fire and muzzle, and `plcmdWeaponSelectMod` for the off-hand weapon.
- **Weapon wheel (user request: reuse the game's built-in wheel):** drive and present the native weapon
  wheel (`plcmdShowWeaponWheel`, native slow-mo, native wheel UI/icons and the `CVRWeaponSelection*`
  remnants) in VR, anchored to the hand, with hand direction feeding the native wheel's selection input.
  A custom 3D WheelRing is only a fallback if the native wheel cannot be driven.
- **Previous wheel idea (fallback):** a hand-anchored 3D WheelRing of native weapon models.
  - Select by pointing or wrist twist, with a haptic tick per sector.
  - The pick is applied through native `plcmdWeaponSlot*` / wheel commands, so it is multiplayer-safe; native slow-mo is optional, via `plcmdShowWeaponWheel`.
  - Use the native `CVRWeaponSelectionWheelInfo` if R3 shows it is live.
- Two-hand steadying.
- **Projected native crosshairs instead of lasers:**
  - Each hand's aim ray hits the world (native ray cast), and the game's own crosshair textures are drawn at that point, depth-correct in both eyes and at a constant angular size: `cp_aptxCrosshairs`, `cp_aptxDispersionCrosshairs`, `cp_ptxLockOnCrosshair`, `cp_ptxZeroDamageCrosshair`.
  - Native spread/dispersion, lock-on, colour coding (`cp_grCrosshairColorCoding`) and the user's crosshair options (`prj_iGameOptionCrosshair`, `hud_fCrosshairScale`) are honoured, so accuracy reads exactly as on desktop.
  - In dual wield there is one crosshair per weapon.
  - Reference: how Fusion VR places its crosshair (`/home/obesecatlord/FastGames/SteamLibrary/steamapps/common/Serious Sam Fusion 2017`).
  - No laser beams; a debug beam is optional and off.
- Per-profile alignment using the native `*_VR` and Knuckles rendering .ep files.

**Verify:** every sector selects correctly; each muzzle is logged per hand; no writes to inventory state; the crosshair sits on the shot's impact point and its dispersion ring matches the native spread.

### Phase 4: Body and multiplayer foundation (before melee and gadgets, to settle authority)
**Local body:**
- Full-body IK, visible locally and in mirrors.
- Use the native VR IK and leg animations if they are live; otherwise our solver writes bone overrides after native animation.

**Gate MP-1, pose transport in order of preference:**
1. Native `ClientActionVR` → `CPlayerVRData`.
2. Lua RPC relay at ~30 Hz, with shot aim in a handshake-gated VR block appended to the native action stream.
3. Steam Networking Messages, presentation only.

**Rest of the work:**
- Handshake: Hello/Ack with protocol version, build fingerprint and content hash. On a mismatch, VR data is disabled for that peer and a message is shown; no one is kicked.
- Remote bodies use received IK, falling back to the native puppet when the pose is more than 0.5 s old.
- Desktop clients have the mod with XR off, native input untouched.
- Dedicated server: no XR or Vulkan; it validates and relays.
- Voice uses native chat.

**Verify (`mp_lab.py`):** two prefixes (simulated VR client plus desktop client) and a dedicated server; logs show pose latency; the server rejects forged aim; the desktop client shows remote IK.

### Phase 5: Physical melee
**Simulation (`core/melee_sim`):**
- The weapon is a rigid body following the controller target with a critically damped linear and angular PD controller.
- Gains and maximum angular speed scale with the native weapon mass, so heavy weapons lag.
- The *simulated* pose is what gets drawn.
- Each frame, sweep the blade/head shape from the previous to the current pose using native ray and hull casts.
- Hitting the world stops the weapon with a haptic pulse and no tunnelling.
- Hitting a character above `vr_fMinSwingToHitSpeed` produces a hit claim.

**Authority:**
- The client sends {target, point, velocity, weapon}.
- The server re-checks it against replicated hand history, speed, cooldown and reach.
- It applies **native** damage, through the `CVRMeleeWeaponEntity` / HitScriptEvent path if live, or the native damage function with the native melee parameters.

**Verify:**
- Spring and sweep unit tests.
- Scripted lab swings above and below the speed threshold.
- Rejections logged in multiplayer.
- Never revive anything like SS2's `unaccepted-melee.patch`.

### Phase 6: Gadgets
**Work:**
- An off-hand gadget wheel reusing the game's built-in gadget selection wheel (`plcmdGadgetSelection`),
  presented in VR the same way as the native weapon wheel.
- The native gadget model is held in the hand (`GadgetHolder.ep`, `*_Arms`/`*_Item`).
- Throwables (BlackHole, NerveGas, Decoy, ClusterBomb, MiniNuke): grip velocity at release, solved with `grenade_throw.hpp`, goes into the native targeting mode and then the native launch mode.
- **Gate R6:** if the launch accepts an initial velocity, pass the velocity directly.
- Activated gadgets (Slomo, CompanionDrone) trigger on squeeze or press while held.
- Detonator uses `bomb_fuse.hpp`.

**Verify:** the landing error between our prediction and the native projectile is logged; the server validates launches.

### Phase 7: Vehicles, rideables, mech (actual vehicle models are the controls)
**Two control modes, both first-class:**
- **Thumbstick driving** (the native vehicle commands) is the default and always works.
- **Grab mode** is an optional, more immersive alternative and can be switched at any time. In grab mode the vehicle's own model is the control surface. Hands grab its real steering wheel, handlebars or levers. No substitute meshes (the SS2 Blender wheel is dropped).

**Steering bones are native data.** Read them per vehicle from the native params:
- `wvsas_idSteeringWheelBone`, `wvsas_bUseHandlebar`, `wvsas_fMaxSteeringWheelAngle`
- `sosd_idSteeringBone`, `sosd_qvInitialSteeringBonePlacement`
- `vp_idSteeringBone`, `mh_idSteeringWheelBone`, `mh_fSteeringWheelAngle`

**Grab and steer:**
- Grab points are taken from the live bone world placement every frame, with rim radius and handlebar grip ends taken from the model's mesh bounds around that bone.
- `steering_wheel.hpp` and `ride_grasp.hpp` keep only the maths: hand angle around the bone axis, two-hand averaging, release and re-grab.
- That angle, normalised by `wvsas_fMaxSteeringWheelAngle`, drives analog `plcmdVehicleSteer*`. The native animation then turns the real bone, and the grabbing hands' IK is pinned to it, so wheel and hands stay locked.
- Throttle: wrist twist on the handlebar grip, or the trigger. Body lean maps to `Lean*`, a squeeze to Handbrake.

**Gate R7:**
- Whether the native steer input is analog, so the bone tracks the hands 1:1. If it is digital only, write the steering bone angle after native animation and drive the steer command with a PWM duty cycle.
- Build a per-vehicle catalog of bone ids and grab extents (`src/games/*`), checked against the Fusion 2017 vehicles where they are shared.

**Mech and rideables:**
- Mech: use its actual cockpit levers or joystick bones where present, with the hand crosshair feeding native turret aim.
- Rideables follow `PlayerRideSelection.lua`; grab the creature's reins or saddle bone if the model has one.
- Thumbstick driving is always available, including mid-drive. Letting go of the grab hands control back to the sticks with no mode switch.

**Verify:**
- Each vehicle's wheel or bar bone visibly rotates with the grabbing hands in eye captures.
- Scripted lab drive for each vehicle.

### Phase 8: UI, HUD, comfort
**Work:**
- A dedicated UI pass rendered into a cylinder-layer swapchain (sharp, independent of game FPS), world-locked with yaw recenter. Backbuffer capture remains as the fallback.
- HUD on its own layer, or the native stereo HUD at `hud_fStereoDepth`.
- A wrist HUD for health, armor and ammo.
- Netricsa and cutscenes on a cinema quad.
- A VR options panel.
- Fixed foveated VRS.
- An optional vignette.

### Phase 9: Platform, packaging, release
**Work:**
- Validation on native Windows with SteamVR and Monado.
- `install.py`: fingerprint check, refuses file collisions, writes a receipt, clean uninstall.
- An optional `.gro`, only if R4 passed.
- `USER_VR_TEST.md` and `MIXED_DESKTOP_VR.md`.

## Performance test matrix (user request 2026-10-23: Vulkan everywhere, measure what is best)
Vulkan is the default and only shipped path unless measurements show otherwise. When the headset is free,
run the same scripted Pompeii scene (fixed route, `+level`, simulated or real Monado) and record GPU
timestamps, CPU frame time and dropped frames for:
- flat baseline: Vulkan vs D3D11 vs D3D12 (stock game, no mod) at the eye resolution;
- VR: two-pass vs engine single-pass (double-wide) stereo, render scale 0.8/1.0/1.2, VRS foveation on/off,
  mirror on/off; Monado/Beyond vs SteamVR.
Results go in `docs/PERF.md`; the default configuration follows the numbers.

## Test lab and verification
**Lab:**
- An isolated prefix copy for each run, plus SS2's Monado simulated pose and input drivers and `runtime_lab.py`.
- An llvmpipe Monado for runs without a headset.
- A JSON scene runner that drives native console commands.
- Eye-image capture from our swapchain, with disparity checks.
- `perf_report.py` reading GPU timestamps.
- `mp_lab.py`: two clients plus a dedicated server.

**Checks:**
- Portable unit tests (`ctest`).
- A symbol-harvest consistency test across all 4 exes, with every signature unique.
- A real-hardware run of `USER_VR_TEST.md` closes every phase.

## Key risks
1. R1/R2 fail (consumers or stereo code stripped). Fallbacks are SS2-style hooks and the Phase 1b injection.
2. wineopenxr or Vulkan interop. Fallback is D3D11 through DXVK.
3. Calling convention between MSVC and MinGW. Hand-lowered functions and ABI tests.
4. Patched-shader hitches. Mod-owned, pre-warmed cache.
5. Render threads versus the simulation thread. Gameplay changes only on the simulation thread; the render thread reads pose snapshots.
6. Wire changes in multiplayer. Only after the handshake, and versioned.
7. Content signing. Add-only, DLL-run Lua.
8. CPU load in large hordes at the Beyond's resolution. Single CPU prepare is mandatory; a stress level is part of the budget.
9. Never commit extracted Lua or game assets.
