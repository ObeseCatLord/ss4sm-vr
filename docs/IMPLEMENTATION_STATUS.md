# Implementation status

## Phase 0 — recon and RE gates
- [x] Repo, AGENTS.md, plan (`docs/PLAN.md`)
- [x] Fingerprints for Sam4, Sam4_Unrestricted, SamSM, SamSM_Unrestricted (`docs/re/builds/fingerprints.json`)
- [x] Registration-table harvest (`tools/harvest_symbols.py`): ~1490 cvars, ~3900–3950 script functions per exe
- [~] R1 device seam: outcome B established; full slot semantics in progress (`docs/re/R1_device_seam.md`)
- [~] R2 stereo path: stereo shader selection live in all three backends (static); runtime capture pending (`docs/re/R2_stereo_path.md`)
- [ ] R3 live VR gameplay
- [~] R4 content mounting: unsigned workshop .gro mount in Unrestricted (`docs/re/R4_content.md`)
- [x] R5 graphics interop: PASS on Proton + simulated Monado (`docs/re/R5_graphics_interop.md`)
- [x] First flat launch of each game (prefixes 257420, 1792250 on GE-Proton11-9); SS4 on Vulkan verified

## Phase 1 — vertical slice
- [x] msimg32 proxy + fingerprint-gated payload load in SS4 (Proton); no startup stall
- [x] Game Vulkan instance/device created through XR_KHR_vulkan_enable2 (multiview feature added)
- [x] XR session on the game's graphics queue (family 0/0), queue access serialised with the runtime
- [x] Milestone "mirror": presented game image blitted into a world-locked quad layer
      (simulated Monado, `docs/evidence/2026-10-23_mirror_quad_monado_sim.png`)
- [x] Same on Siberian Mayhem (private display, `docs/evidence/2026-10-23_sm_mirror_private_display.png`)
- [ ] On real Beyond/Monado; on SteamVR
- [x] Frame pacing root cause: the unfocused game window is capped by `gfx_iMaxFPSInactive` (default 20;
      0 means 1 fps, not unlimited). With it at 500 plus `gfx_iMaxFPSActive 0`, `men_iMaxAllowedFPS 0`,
      `gfx_iWaitVSyncs 0`, the mirror holds the runtime rate (60.0 fps on simulated Monado).
      TODO: the payload must set these in memory whenever VR is active (window is usually unfocused).
- [x] OpenXR input layer (`src/xr/input`): Index/Touch/Vive/WMR/simple profiles, aim+grip poses with
      velocities, trigger/squeeze/stick/buttons with edges, haptics; verified with simulated Index controllers.
- [ ] Device seam (`COpenXRDevice` at the VR device global) — needs R1 slot semantics
- [ ] Engine stereo rendering into the projection layer (R2)

## Phase 2 — input (in progress)
- [x] Virtual XInput pad from VR controllers through the game's own gamepad bindings: title/menu
      navigation, chapter screen, and in Pompeii: move (left stick), turn (right stick), fire (right
      trigger) — `docs/evidence/2026-10-23_pompeii_move_turn_fire_vr_controllers.png`
- [x] Game window kept "active" while VR runs (engine ignores input when its window is inactive)
- [x] Lab: `+level <path>` loads a level directly; `tools/lab/xkey.py`/`xfocus.py` for the private display
- [ ] Hand-tracked aiming and head-tracked view (needs the device seam, R1 table in `docs/re/R1_vtable.md`)
- [ ] Final controller layout with native weapon/gadget wheels (Phase 3)

## Offline progress (2026-10-23, headset busy)
- Device: eye render targets through engine holders, Vulkan eye-image capture, projection layer with the
  render-time views; vibration list (0x130); button table with engine edge flags (0xd0/0xd8/0xe0).
  RE notes: `docs/re/EYE_TARGETS.md`. SamSM slot usage verified identical (`docs/re/R1_vtable.md`).
- Core (host-tested, `ctest` in build-tests): pose maths, engine projection, physical melee simulation,
  full-body IK, 64-byte networked pose packet, grab-to-steer, ported SS2 two-hand/grenade/motion/comfort.
- Installer with receipt (`tools/install.py`); VR opt-in switch; virtual pad only with an XR runtime.
- Pending hardware/runtime checks: see `docs/USER_VR_TEST.md`. Gate R3 (live VR gameplay) in progress.

## Offline progress, continued (2026-10-23)
- Gate R3 done (`docs/re/R3_vr_gameplay.md`). The engine's VR player layer (`CPlayerVRData`) is dormant but
  callable.
- `src/se4/player_vr.cpp` turns it on for the local VR user:
  - Forces `SetPlayingInVR(1)` at the local-user binding.
  - Guards three unguarded engine paths: the vrd_init slot 0x208 thunk, the controller placement for a
    NULL vrd, and `ClientActionVR` for a player without VR data.
  - Reads the player→vrd offset from the running build (SS4 0x25c8, SM 0x25d0).
  - Expected result: hand-placed weapons, shots from the muzzle and a head-tracked camera. Not yet run.
- Fixed a latent input bug. The device used to set +0xb3c on a trigger press, which switches the engine to a
  "VRController" input device SS4/SM do not have. It now forces `vr_bVRControllerExclusiveInput` instead,
  restored at shutdown.
- Controls: `core/vr_controls.hpp` gives a per-profile layout (Index, Touch, Vive, WMR, simple) on the virtual
  gamepad. Holding the wheel button turns hand movement into native weapon-wheel selection for each hand.
  Host-tested (`controls_checks`).
- Still to do for the wheel: drive the gadget ring (syscmdUp/Down) from the controllers, and present the wheel
  on the HUD layer near the hand.
- Blocker found by `tools/lab/check_log.py` in the last lab log:
  - The engine never called device slots 0x028, 0x030 or 0x018, so its VR view render (0x1401dcbe0, reached
    only through CProjectInstance slot 0x58 at 0x1400552e0) never ran. That run never got to stereo.
  - What gates that call is unknown. Likely an SS4 constant flip, like `SetPlayingInVR` (Fusion
    `prjIsVRActive` = vt[0x18](this,1); SS4 = vt[0x10](this,0)).
  - Gate R10 is in progress (`docs/re/R10_vr_render_gate.md`).
- In progress: gates R7 (vehicles), R8 (crosshair), R9 (body IK) and R10.
- Gate R8 done (`docs/re/R8_crosshair.md`). The game's own crosshair is now enabled for hand aim:
  - `src/se4/crosshair.cpp` makes the five calls to a hidden always-true predicate answer 0 for the local VR
    player.
  - The aim ray then comes from the barrel, and the native crosshair and dispersion ring are projected onto
    the barrel's hit point.
  - Shots already travel along the barrel for bullets, beams and projectiles. The exception is the
    auto-shotgun while zoomed.
  - Still to do: per-eye correctness (R8 s8 B: projection and eye offset during the crosshair draw, once R10
    shows which stereo path runs), and a second crosshair for dual wield (s8 C).
- Gate R10 done (`docs/re/R10_vr_render_gate.md`). SS4/SM were compiled to call `CProjectInstance::Render`
  (slot 0x50) from the main loop. Only Fusion's VR exe calls `RenderVR` (slot 0x58), and no flag or cvar
  switches it.
  - `vrdev::hookRender` detours Render. While our device is installed, the main loop's call (inVR 0, return
    address main_loop+0x25f, verified in all four builds) goes to RenderVR. Per-eye renders (inVR 1) pass
    through.
  - `vr_bForceImmersiveMode` is set to 1 so the world renders in stereo, not on the cinema screen.
  - Harvester fix: records whose first qword is string bytes are now accepted as variables when every other
    field validates. This added 31–32 real cvars per build, among them SM's `vr_bForceImmersiveMode`, with no
    existing address changed.
  - The harvest also lists a `_svc_AppInputDeviceVRController` service in every build. A "VRController" input
    device may exist after all, contrary to the R3 follow-up. Check at runtime before relying on either
    conclusion; the device still never sets +0xb3c.
- Gate R7 done (`docs/re/R7_vehicles.md`). Native steering is analog, so thumbstick driving already works
  through the virtual pad's left stick. The `plcmdVehicle*` commands are dead in these builds.
  - Grab mode (`src/se4/vehicles.cpp`, option `vehicle_grab`, default 1):
    - Squeeze near the vehicle model's real steering bone.
    - The hands' angle about the bone's heading axis becomes the native steer (pad LX = −steer; the stick wins
      when deflected).
    - The bone is posed 1:1 through the vehicle's own bone follower after the native animate step.
    - Supported where the content has a cockpit wheel or handlebar bone plus the `Left/RightHandSteeringHandle`
      attachments: SS4 ATV; SM SpecialATV, Crane, Snowmobile; Harvester. The chopper, tank and mechs stay
      thumbstick-only for now.
  - Member offsets now come from the engine's reflection records per build (`tools/members.txt` →
    `tools/harvest_members.py` → `docs/re/builds/members.json` → `Build::off`).
  - Lab checks: steering sign, the heading axis logged at first drive, and that the hands stay on the grips.
- Gate R9 done (`docs/re/R9_body_ik.md`). The engine's VR body system (room-scale legs, head/hand adjusters,
  VR hand models) is live code with no content in SS4 or SM, so full-body IK has to come from the mod.
  - Planned approach: a mod-owned motion handler registered last on the player model's animation queue
    (0x140bf6b20; called by 0x140bad5b0 after the blend). It writes parent-relative bone quaternions solved by
    `core/body_ik.hpp` from the vrd poses, on every machine.
  - Local first-person body: hook the hide-the-viewer setter 0x140ca3280 and use the `FirstPersonAvatar` tag
    the way the engine does.
  - New guard, done: the slot 0x1d8 forwarder (Sam4 0x14043aac0, SM 0x14043ac40), which runs every tick for
    every player with vrd, now goes through the null-checking thunk.
- Body IK, first version (`src/se4/body.cpp`, option `body_ik`, default 0 until verified):
  - Keeps one engine motion handler per player with vrd, registered from the vrd tick on the player model's
    animation queue.
  - After the native blend it re-aims both arms (`core::retargetArm`, host-tested) so each
    `Left/RightWeapon` bone lands on that hand's controller placement. That placement is replicated for
    remote players, so other machines see it too.
  - Legs, hips, spine and head stay native.
  - Still to do: torso and head from the HMD, a first-person body (R9 s5B), desktop peers (needs `mp_guards`
    plus `body_ik`), and keeping the handler after `CHandsAdjuster` when that adjuster is re-created.

## Lab session 2026-10-23 (simulated Monado) and follow-ups
- First stereo in the lab: the SS4 start screen, menu and loading screens render in both eyes through the
  engine's own VR view at the simulated 60 Hz.
- Fixes found by running:
  - Single pass crashed: the engine's double-width target came back NULL. Two-pass is now the default, and
    single pass sits behind the `single_pass` option.
  - The engine released the eye targets after about 4 s. They are now re-created every call, as the engine
    does for its own.
  - Lab keyboard input needs a click first (`tools/lab/xclick.py`).
- Open issues from the lab:
  - The loading screen's Continue doesn't respond in VR, so the player never spawned and the local vrd was
    never created.
  - The game never polled XInput because the pad was missing at its first enumeration. Fixed in code by
    starting the XR runtime at XInput resolution; retest.
  - Menus are head-locked.
- Lab safety:
  - A lab Monado left running took the user's IPC socket. The lab now uses a private `XDG_RUNTIME_DIR`, and
    `tools/lab/stop_lab.sh` ends every session.
  - Level starts write quicksaves to the user's Steam cloud folder (14 lab saves, numbers 401–414; the 2021
    saves are intact). Both games' saves are backed up in `lab/save-backups/`.
- Foveated rendering (`src/vk/foveation.cpp`), untested:
  - Mechanism: a VK_KHR_fragment_shading_rate attachment on the scene passes (render passes with depth and
    colour). Eye-size framebuffers get the pattern; the engine's own VRS combines with it (the coarser rate
    wins).
  - Default: dynamic, eye-tracked (XR_EXT_eye_gaze_interaction), on a Steam Frame with gaze; off elsewhere.
  - Options: `foveation=0|1|2` (off, fixed, dynamic) and `foveation_inner/outer` (percent of the image height).
  - Quad views were evaluated in `docs/research/DLSS_FOVEATION.md` and deferred (they would need a second scene
    draw).
- Research: neither game has DLSS/FSR/XeSS or TAA, and no community upscaler mod exists. Upscaler plan: per-eye
  FSR 3.1 and DLSS/DLAA using the engine's dormant motion-vector shader path, with added jitter.
- Single pass re-analysis: the crashing call used the device's eye-1 target (vr_eye_render takes it when
  device+0x1c is set, device+0x18 == 0 and +0xb1c == 0), which was NULL only because of the
  released-target bug fixed above. Retest `single_pass=1` in the lab; it may work now.
- RE gates started: R11 (VR menus and loading-screen Continue), R12 (melee and gadgets), R13 (multiplayer
  mod channel/Lua bridge and upscaler insertion).
- Upscalers (`src/up/`):
  - DLSS initializes in the lab under Proton with Vulkan. It goes through the driver's NGX core using a
    custom-engine project ID: `NVSDK_NGX_VULKAN_Init_ProjectID_Ext`, with the argument order read from
    nvsdk_ngx_s.lib. A numeric app ID gave FeatureNotFound.
  - The parameter vtable self-test passes.
  - FSR 3.1 loads from `amd_fidelityfx_vk.dll`. It's the fallback, and the default on non-NVIDIA GPUs.
  - Not yet wired into the eye render: that needs R13's insertion point, motion vectors and jitter in the
    scene projection.
  - Options: `upscaler` (0 off, 1 FSR, 2 DLSS, -1 auto), `upscale_quality`, `upscale_sharpness`.
- Fixed-foveation lab run (`foveation=1`): scene passes get the attachment, the 84x95 rate image is created,
  and the menu renders correctly.
- Lab fix: a lingering lab wineserver kept the mod log locked. `killgame.sh` now stops lab wineservers, and the
  log falls back to a per-process name.
