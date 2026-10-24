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
