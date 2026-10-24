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
