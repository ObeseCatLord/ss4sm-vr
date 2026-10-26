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
- Lab, 2026-10-24:
  - VR gameplay works in SS4 Pompeii: the local CPlayerVRData exists, the weapon follows the right controller, the
    left arm steadies two-handed weapons, the head turns the view, and the trigger fires.
  - Menus, loading screens and cutscenes use the engine's cinema screen now that immersive mode is no longer
    forced (`force_immersive=1` restores the old behaviour). The screen re-centres on head yaw. A comfort-anchored
    OpenXR layer is still to do (R11 s3).
  - Siberian Mayhem reaches its menu in stereo.
  - The pad is polled, and A passes the loading screen's Continue.
  - New goals: picture-in-picture scopes (R14) and separate hands with finger tracking (R15).
- Physical melee, knife (`src/se4/melee.cpp`, R12):
  - For VR owners the knife's canned arc is skipped. After each knife step the hand's blade is swept with the
    native segment cast.
  - Hits above `vr_fMinSwingToHitSpeed` apply `CDamage` exactly like the knife's swing test, once per target per
    0.35 s. The engine counts damage only on the authority.
  - Lab: the shotgun swings with no crash. The knife hit check is still pending: in VR the weapon-slot keys don't
    reach the game, so the knife needs the wheel or the pad.
- Lab: the lab Monado compositor can wedge after many sessions (no window, no session). `stop_lab.sh` and a
  restart fix it. One transient Vulkan "cannot create main swap-chain" failure fell back to D3D12 and stopped
  the game. NGX extensions are now added only when an upscaler is selected.
- R13 done: CNMScriptRPC raw byte channel for the mod handshake and poses; upscaler insertion before post FX
  at 0x140c89010 on the HDR canvas; motion vectors to be synthesized per eye from depth.

## Lab session 2026-10-24 (continued): upscalers in the eye render, scopes, two-hand grip
- Upscalers (`src/up/integrate.cpp`, R13 phase A), working in the lab:
  - DLSS (DLAA, preset K) and FSR 3.1 native AA run per eye inside the engine's command buffer and the game keeps
    running.
  - Runtime facts behind the integration:
    - The HDR canvas (RGBA16F) and the scene depth (D32, **reverse-Z**: cleared to 0) are created inside
      0x140c88a10. The mod adds SAMPLED usage to the depth.
    - The engine's command buffer is at `gfx+0x1ea8`, validated by the loader's dispatch magic. It isn't bound to
      the calling thread.
    - Each eye makes two post-processing dispatches; only the `flags 0x1e` one reads the canvas, and the mod learns
      which one at runtime.
    - The scene pass is still open at dispatch entry, because the engine ends passes lazily. The mod therefore
      records its work at that dispatch's first canvas barrier (COLOR_ATTACHMENT → SHADER_READ).
    - Scene depth passes already store depth and stay in DEPTH_STENCIL_ATTACHMENT_OPTIMAL.
  - Motion vectors: camera-only, from depth (`src/up/shaders/motion.comp`, `core/reproject.hpp`). The camera is the
    head world placement (0x14043cb50) composed with the eye in head, and depth decodes with the slot-0x070
    near/far. Host tests cover the depth decode, reprojection and jitter.
  - Jitter: Halton(2,3), applied in device slot 0x070 by shifting the frustum tangents. Lab option
    `upscale_jitter_x/y` flips the sign reported to the upscaler for the convention check.
  - Lab images: a still view and a mid head-turn view, with no ghosting on edges. A stability/sharpness matrix
    (`tools/lab/upscale_matrix.sh`) settles the jitter sign.
- Picture-in-picture scopes (`src/se4/scope.cpp`, `src/vk/scope_layer.cpp`, R14), built, lab check pending:
  - The engine's third view (eye 3) renders the scope camera. The device raises `vr_iMixedRealityOutput` only in
    frames with a live scope and answers slots 0x028/0x070/0x078 for eye 3.
  - Camera: barrel placement. FOV: player base FOV / `swmp_fZoomFactor`.
  - The image is transitioned with gfx vt+0x288 and shown as a circular OpenXR quad at the eyepiece of the
    weapon's hand, with a reticle.
  - Live when natively zoomed or when the eyepiece is within 20 cm of the head (option `scope=2`: always).
- Two-hand grip by squeeze (`src/se4/hands.cpp`, R15 s4.3), built, lab check pending:
  - The weapon step's grip bit (the other hand's fire bit, player +0x2338 / SM +0x2340, read from the step's code)
    is replaced for the step only with the grip intent. The grip engages on a squeeze press edge within the
    weapon's snap distance of `LeftHandGripEffector` and holds until the squeeze is released.
  - The off hand's squeeze is withheld from the pad while that hand is in reach or gripping.
- Finger poses (`core/hand_pose.hpp`, host-tested): controller-state curls and OpenXR joint flexion, smoothing, and
  the wire packing. Not yet applied to bones.
- Upscaler image quality (lab, 2026-10-24), open item:
  - Facts:
    - The rendered jitter has the expected magnitude: a 10× lab scale shifts the image ±5 px.
    - Every evaluation uses the same jitter the eye's projections had in that frame (0 mismatches in 3000).
    - The NGX parameter shim is correct; cross-type reads convert.
    - DLSS uses the reported jitter: reporting it 10× too large wrecks the output.
    - The camera is static and there are no history resets.
  - Symptoms:
    - DLSS with jitter is stable-ish (frame diff 0.5 against 0.2 for no AA on a static region).
    - It is noticeably softer than no AA, and about 25 % softer than a 6-frame supersampled reference. Fine
      corrugation fades.
    - DLSS without jitter is as sharp as no AA.
    - Sign flips and ±1 frame jitter lag give results within run-to-run noise. FSR prefers the current sign
      convention.
  - Next steps:
    - Use NGX's debug overlay (ShowDlssIndicator, jitter/MV views) in the lab.
    - Make a deterministic static capture: freeze foliage/time and read the eye image back directly instead of the
      desktop mirror.
  - Texture LOD bias (R23, 2026-10-25): −1 on the scene binds while upscaling (`upscale_lod_bias`).
  - A/B, 2026-10-26 (`tools/lab/upscale_matrix.sh`, still headset view, Pompeii, 6 frames, left eye):

    | config | frame diff | sharpness (Laplacian var.) |
    |---|---|---|
    | no AA | 1.29 | 4494 |
    | DLSS (DLAA, preset K), no bias | 1.04 | 3419 |
    | DLSS, bias −1 (default) | 1.06 | 3541 |
    | FSR 3.1 | 1.73 | 3861 |

    The bias gives DLSS about 4 % more detail at no stability cost. DLSS removes about 18 % of the frame-to-frame
    shimmer (foliage moves in the wind, so the floor is not 0) but stays softer than no AA. FSR shows low-level noise
    over static walls and ground, so its jitter is not fully resolved (sign flips were tested earlier).
    **Resolved later on 2026-10-26.** After the phase-B rework of the FSR inputs (render size and resource extents),
    FSR measures a 1.29 frame diff and 4198 sharpness, against 1.33 / 4496 without AA. Its ground and wall difference
    dropped from 2.42 to 1.47, and the static noise is gone from the difference map. The reverse-Z depth convention
    (lab switch `upscale_fsr_depth` 0/1/2) makes no measurable difference.
  - `upscaler` stays 0 by default: the sharpness/stability trade-off is a headset decision (USER_VR_TEST). Lab switches are listed in src/up/integrate.cpp.
- Lab, 2026-10-24, SS4 Pompeii, hands and scope working:
  - **PiP scope:** the sniper's scope view is a circular, magnified image with a reticle at the eyepiece, in the
    weapon's hand. The engine renders it as eye 3.
    - Fixes needed: gfx vt+0x288 takes `*rt`, and the layer is premultiplied. Monado showed nothing with
      unpremultiplied blending.
  - **Free off hand:** the left arm reaches its controller (IK from a virtual shoulder, finger curls) and replaces
    the native controller model. The controller objects are found at vrd+0x4e0/+0x3d8 from the Hide*VRController
    scripts.
  - **Two-hand grip:** squeezing the left hand at a rifle's foregrip engages the engine's own two-hand blend; the
    rifle follows the main-hand→off-hand line, and releasing the squeeze returns it to one hand.
    - No SS4 weapon has `wp_bTwoHanded` set, so the mod raises it only around the weapon step and placement. That
      applies to weapons whose LeftHandGripEffector is at least 12 cm from the hand, so dual-wield rules are
      unchanged.
    - The native grab test never grabs: its attachment virtual finds nothing. The mod grabs exactly as the engine
      would, with the one-hand pose, the time stamp (entity time 0x140ba78c0) and flag 0x20, and the native release
      path is kept.
  - **Weapon wheel per hand (2026-10-24):** on Index/WMR each trackpad held opens the native wheel for that
    hand's weapon only (the other hand's stick is held neutral); a right tap (< 250 ms) toggles dual wielding,
    the left tap stays previous weapon. Lab: right wheel picked the pistol, the left wheel the second pistol
    (dual wielding is a skill: lab give-all sets it).
  - **Weapon wheel:** the Index left trackpad press opens the native wheel. As device button 6 it had opened
    Netricsa, so the press is no longer reported there. Lab weapon selection is in `tools/lab/to_sniper.sh`, and give-all
    needs `+cht_bEnableCheats 1`.
- Lab, 2026-10-24:
  - **VR menu pointer works.** The right controller's aim ray hits the cinema quad. Menu items highlight under it
    and the trigger clicks: the main menu opened Options. Seen from the viewer, the quad's local +X runs right to
    left, so u is flipped for rays from its −Z side.
  - **Siberian Mayhem Refinery:** the free left hand, two-hand grip (the pistol counts as two-handable: foregrip
    0.16 m) and the crossbow's PiP scope all work. Fire bits are at +0x2340 and the player spawns after 46 s.
- **HUD, crosshair and wheel panel in VR (2026-10-24, lab SS4 Pompeii):**
  - Cause of the missing HUD (R8 A5/A6): with the device immersive, the HUD render takes a 3D "VR panel" branch whose
    per-element placements SS4/SM never authored (identity, 0, stretch 1: pixels became metres), simple elements set
    their own matrix, and queued elements share the first entry's matrix. Also the show predicate hid the crosshair
    for every player with vrd.
  - Default now (`src/se4/hud_vr.cpp`, hud_vr_mode=0): the HUD render's immersive-flag call (+0x1e1) answers 0, so
    the HUD draws in 2D per eye like on desktop: armour, health, ammo, gadget bar, objective, portraits and the
    beacon marker show in both eyes; world markers project per eye. Pulling edge elements in is open (the safe
    area cvars did not move the layout).
  - Crosshair (`src/se4/crosshair.cpp`): show predicate's vrd test NOPed; its view call gets the eye offset, so the
    game's own crosshair is drawn where the barrel ray hits, at that depth (seen on the hut, small disparity).
  - Weapon wheel (`src/se4/wheel_panel.cpp`): its dummy menu screen no longer sends the view to the cinema screen
    (immersive forced for those frames); while it is open the HUD keeps its 3D branch and the wheel is drawn on a
    panel in front of the opening hand (both trackpads: between the hands). The world blur/tint post-process is
    skipped (user request; `wheel_blur=1` restores it).
  - Weapon placement checked: the weapon root equals the controller pose (identity rotation, offset ~0.1 m from
    the weapon's own vt+0x768 offset) and the barrel lies along its -Z; the visible hand sits at the controller.
    (A gun below eye level looks "pitched up" on screen only through perspective.)
  - Gadgets: throws need the native target check (too close / invalid: kept in hand, aiming hand buzzes); the
    activation's own targeting pass is steered by the aiming hand; refused activations re-arm. SM: injection,
    tosses and a Nerve Gas launch verified; Mini Nuke refused when aimed 2 m ahead (native too-close check).
- **Melee, kick and grips (2026-10-24):**
  - Right B = use / melee kick (desktop E): the mod calls the engine's own VR use path for the right hand on the press
    (independent of the saved pad layout); lab: the call happens, a kick on an enemy is unverified. Crouch moved to
    the right stick click; the right grip also jumps (a grip busy with a two-hand hold does not).
  - Physical melee covers the knife and the generic melee weapons: five blade points swept frame to frame plus head
    rays through the swung arc out to the weapon's native swing radius (melee_arc). Unverified on enemies.
  - No crosshair while a melee weapon is in the right hand.
  - Open: Sirian Artifact of Might crush (hold the native "Use" artefact animation before its ArtifactBroken event
    until trigger / Index squeeze force / Frame grip second stage); Steam Frame controller profile (grip stages).
- Gadgets (`src/se4/gadgets.cpp`, R12):
  - Pad buttons depend on the player's saved layout: pad Y opened Netricsa in the lab profile.
    - The left squeeze now issues the player's own quick-use HUD command (OnHudCommand(1, 0xc), the path
      plcmdGadgetQuickUse takes).
    - While the weapon wheel is held, right A/B cycle the gadget ring (HUD 0xd/0xe) instead of D-pad buttons.
    - Lab: the medkit was used.
  - The device no longer reports buttons 1–6 to the engine unless `native_vr_action` is on. The VR player code read
    them in levels and acted on them too.
  - **In-hand gestures (user design 2026-10-24, `core/gadget_use.hpp`), lab-verified in SS4 Pompeii:**
    - The left squeeze takes the selected gadget into the right hand (HUD 9 → GadgetHolder, no quick use). Again:
      put away by selecting the weapons held before through the wheel's net commit 0x140471450 (HUD 9 with the
      holder up cycles to the next gadget; a wheel-button tap does not leave the holder).
    - Classification from the item's name key (gip_tsName +0xc0, a char* in all builds) and bot usage type
      (+0x188): MedKit inject (other forearm, or the torso/hips with the needle pointing in), SeriousRage drink,
      ExtraLife chest, CompanionDrone/SeriousTime toss, FIRE_AT_FOE_* aimed throw, anything else the trigger.
    - Aimed throws: while the AOE targeting step 0x140a62220 runs, the player view (vt+0x340 mode 0) is the aiming
      (left) hand's orientation, placed so the first-person start offset (params+0xe0) lands at the gadget hand.
      The native arc, marker, target checks, speed and lifetime follow; captures show the arc swinging with the
      left hand. A throwing motion (core/grenade_throw) issues HUD 0xc → holder use → native launch.
    - Lab: Black Hole/Nerve Gas/Decoy/Mini Nuke launched (speeds 40/50/25/10), drone and Serious Time tossed, the
      MedKit injected into the forearm and into the hip, Serious Rage drunk. Extra Life is not in the selectable
      ring (it is not manually selectable).
    - Lab give-all now also sets the cheats menu's give-all flags (gadgets, upgrades, skills): found in
      CMSCheats2::GiveAllGadgets_OnChange's code.

## 2026-10-24/25: crosshairs per weapon, artifact, Frame, wheel spots, menus, prompts, isolated lab
- **Isolated lab (user request 2026-10-25):** `tools/lab/make_lab_tree.sh` builds `lab/game/<ss4|sm>[-n]`, a
  symlink mirror of the install with the gbe_fork Steam emulator (offline, LAN multiplayer), its own saves and
  profile (`UserData/`), logs, UserCfg.lua and Wine prefix (`lab/prefix/<name>`). Lab runs no longer touch Steam,
  the user's cloud saves/profile, the game folder or its compatdata prefix (verified after a run). Earlier lab runs
  had written QuickSaves/profile into Steam userdata and UserCfg.lua into the game roots (2026-10-23); a restore
  attempt desynchronised Steam's cache and the SS4 profile was reset (copies kept in `lab/`, see LAB_NOTES).
- **One crosshair per weapon (R8 A7):** the crosshair element runs once per hand holding a gun, each pass reading
  that weapon, its barrel target (left barrel cast after the native aim writer), hit entity and own spread. Melee
  weapons draw none. Lab: dual pistols show two crosshairs following their hands; a minigun's gap is wider than a
  pistol's; knife right + pistol left shows only the left one. Stereo: at 3 m the crosshair's disparity equals the
  ground's (-6 px), so it sits at the target depth in world space.
- **Melee:** knife swings killed Processed in Survival (blade and arc hits). The right-B kick cleared Survival wave 1
  with no weapon in hand and fire never pressed (player kept alive by the lab Lua), so the kick hits enemies.
- **Scope crash fixed:** the PiP scope used a freed weapon after a weapon switch; it now checks the player still holds it.
- **Sirian Artifact of Might (R17):** the native artefact "Use" animation is held in the hand (anim queue speed ~0
  at 1.5 s, end timer cancelled and re-armed on release) until crushed (trigger, Index squeeze force, Frame grip
  second stage); timeout 120 s. Lab (SS4 and SM, animation started by the lab Lua bridge): the artefact is held
  in the hand (19.6 s / 7.1 s), a trigger crush resumes it and ArtifactBroken fires natively ~1.5 s later.
- **Lab Lua bridge (R20):** `Bin/x64/ss4smvr/lab.lua` in a lab copy runs once in the local player's world (engine's
  own lua_exec_string in the world script context) after `lab_lua_delay` ticks. Lab copies only.
- **Steam Frame controllers:** XR_VALVE_frame_controller_interaction profile; bumpers hold the per-hand wheels,
  ABXY/D-pad/View mapped, grip second stage = crush.
- **Wheel spots (R18):** moving the wheel hand past the wheel to the side points at Menu / Netricsa (left) or
  Quick load / Quick save (right); releasing there runs the game's own Fusion VR wheel button functions. Lab: all
  four work (quick load shows the native confirmation; saves go to the lab's UserData). Labels: drawn per eye on the 2D path (hand-panel text, below).
- **Menu layout:** while a menu or Netricsa is open, A confirms, B goes back/closes, grips (Index trackpads, Frame
  bumpers) switch tabs, the trigger clicks through the pointer. Lab: Netricsa tab switch and close.
- **Button prompts (R19):** the virtual pad's control markup returns the VR control (Xbox glyph where it matches,
  text otherwise) per profile and layout (`core/prompts.hpp`); prompts asked of the keyboard device go through the
  pad router; menu button bars run in gamepad mode while the VR pointer moves the mouse. Lab: HUD prompts (plcmdFire
  → RT) and the pause menu bar "RESUME GAME (B) · CHEATS · SELECT (A)" on Index.
- **Siberian Mayhem:** per-weapon crosshair, wheel spots, Netricsa controls, prompts and the artifact crush checked.
- Open:
  - upscaler quality (see the upscaler item); multiplayer (later: gbe_fork / SmartSteamEmu, remote server).
- **Hand-panel text (2026-10-26, R18 s7.4):** the engine's 3D HUD path draws no text, so the wheel on the hand showed
  neither the spot labels nor its own title, gadget names or ammo counts. While the wheel renders on the hand, its
  text calls are projected per eye onto the 2D path at the panel's depth (`hud_vr.cpp panelText`, option
  `wheel_panel_text`). Lab (SS4 Pompeii): spot labels on their boxes, "GADGETS" and the gadget names on the panel.
- **Vehicles (lab, 06_Countryside Harvester):** boarding no longer crashes (the native VR locomotion's unchecked
  physics-body read answers "not airborne" while riding, so the VR anchor keeps following the seat); seated height on
  boarding (`ride_eye_cm`, default 80); grab steering falls back to an 18 cm rim when the model has no grip
  attachments; the wheel was grabbed and turned (0.5 rad), the stick drove it, the use button got out.
- **Lab suites (2026-10-26, `tools/lab/suite.sh`, isolated lab copies):**
  - **SS4: 20/20** over the core, vehicle, survival and artifact stages.
  - **SM: 20/20** over the core, vehicle, melee and artifact stages. SM has no Survival map, so the melee stage gets
    its enemies from the level's spawners through lab Lua.
  - Per-game wheel directions come from `tools/lab/wheel_map.sh`, which uses the new "hands: right hand weapon" log
    line. In SM the scoped weapon is the crossbow.
- **Riding view (R24):** seats that prefer the chase camera (SS4 ATV, chopper and tractor; SM snowmobile) now keep
  the VR rider in first person at the seat. The puppet predicate "is in third-person view" answers first person for
  the local VR rider (`ride_first_person`). Lab: SM snowmobile first person with the hands on the handlebar grips.
- **Multiplayer channel (R13 s3), lab-verified in SS4 on 2026-10-26 (dedicated server + VR client + desktop client):**
  - Code: `core/net_proto.hpp` (protocol, `tests/net_checks.cpp`) and `se4/net.cpp` (transport).
  - Mod messages travel as the engine's `CNMScriptRPC` under mod-private name hashes that are never interned, so an
    unmodded peer only logs one line. `CNMScriptRPC::Process` is detoured to consume them.
  - Handshake: Hello with protocol, build, `.text` CRC, `en_iPlayerIndex` and caps. The server answers that client
    alone with an Ack (`server+0xd790` sender slot, send-one). A refusal disables the channel for that peer and kicks
    nobody.
  - Poses: 68-byte packets (head and hands relative to the player, grip and trigger) are sent unreliably at about
    30 Hz after an accepted Ack. The server relays them to the other acknowledged clients; a listen host sends its
    own.
  - Desktop peers (`mp_guards=1`, `install.py --mode desktop`) say Hello from a remote VR player's tick. Options:
    `mp_channel`, `mp_log`.
  - Lab tooling: `tools/lab/mp_lab.sh` runs a dedicated (or listen) server, the VR client and a desktop client as
    isolated gbe_fork copies over 127.0.0.1. `run_game.sh VR=0` runs flat instances.
    - The server is started from its console at the main menu with `gamStartServer(...)`, then `gam_*` cvars, then
      `gamStart()`. `gamStart()` alone starts single player, and a command-line `+exec` runs before the game's
      functions exist.
    - Clients join with `+gam_strConnectToIP`.
  - Lab result (SS4 Pompeii co-op):
    - The VR client is marked as playing in VR and gets its local vrd. Its Hello is acknowledged, and its poses
      reach the server, which relays them.
    - The desktop client (`mp_guards`) is acknowledged and receives the VR player's relayed poses.
    - Both clients see each other in the level. There was no crash or kick.
    - The engine logs a non-fatal "Code guard was activated" line on the clients and in single player too. It is
      not specific to multiplayer.
  - Later the same day:
    - SM passed the same three-way test.
    - A VR client of a remote server now sends `ClientActionVR` automatically.
    - Native turning works (R26: three read redirects, `src/se4/turn.cpp`), and the anchor yaw is folded into the head
      that is sent.
    - Desktop-guard peers (the dedicated server) sweep remote VR players' melee. The server counted 12 knife hits from
      the VR client (`MELEE=1 mp_lab.sh`).
    - Final single-player regression on the current build (single pass default): **SS4 21/21, SM 21/21**.
  - Polish (2026-10-26, docs/MULTIPLAYER_TEST.md §3):
    - Remote VR players' finger curls are relayed and shown on their third-person hands.
    - Body IK is on by default, desktop peers included.
    - The server-side facing follows native turns (checked in SS4 and SM).
    - The engine's "Code guard was activated" log line also appears with the mod not loaded (`run_game.sh NOMOD=1`)
      and in the user's own Steam logs. It is a vanilla warning under Proton.
  - Still open:
    - The internet test (docs/MULTIPLAYER_TEST.md §2: a remote server with gbe_fork `custom_broadcasts.txt` or a
      direct IP, and port forwarding).
- **Performance (docs/PERF.md, 2026-10-26):**
  - New tools: `perf_timing` (GPU and CPU ms per frame in the log), `render_scale`, and `tools/lab/perf_matrix.sh`.
  - Single pass works again. It saves about 15 % of game CPU time.
  - Its HUD was missing because the HUD renders with the device's eye field at 0. The HUD now finds its eye from the
    drawport that slot 0x028 returned (`vrdev::hudEye`).
  - Upscaler phase B (lower internal render size) is open: R25.
- **Desktop upscaler menu (R28, 2026-10-26; built and host-tested, not yet lab-verified):**
  - Opt-in `desktop_upscaler=1` (VR off; `install.py --mode desktop` writes it with `mp_guards=1`). Without it flat
    play is unchanged (no hooks).
  - The game's own Options > Graphics > Rescaling Filter list gets "DLSS" (NVIDIA with NGX only) and "FSR 3.1"
    after the six stock filters (`src/se4/gfx_menu.cpp`). Choosing one switches the flat upscaler at the next frame
    boundary, no restart; a stock filter or Default turns it off. `ren_iRescalingFilter` is never written: the mod
    swallows its own values in `wlink_set` and reports them in `wlink_get`. The row stays visible under the MPix cap.
  - Both backends are initialised at startup in this mode (`up::select` switches; the other backend keeps its
    contexts). The choice is saved in `ss4smvr/upscaler_menu.txt` (`upscaler=off|dlss|fsr`). `upscaler` in
    options.txt, when set, overrides that file at startup; the menu still switches at runtime.
  - Quality = the game's "Max 3D rendering Mpix": the flat limiter uses the engine's own result (at least a third of
    the output per axis). Equal to the output gives DLAA / FSR native AA (phase A); smaller gives phase B (upscaled
    to the output, the engine's rescale skipped). `upscale_quality` in options.txt keeps the mod ratio instead.
    Jitter phases, LOD bias and the DLSS quality mode follow the actual ratio.
  - Log lines: `menu: Rescaling Filter entries on`, `menu: rescaling options appended`, `menu: rescaling select`,
    `upint: flat upscaler X -> Y at a frame boundary`, `upint: flat limiter (menu ...)`. Lab checks: R28 s6.
- **Upscaler menu, part 2 (R29, 2026-10-26):**
  - **Sharpening.**
    - DLSS has no sharpening of its own any more, and FSR 3.1's own RCAS is now always off. The mod runs AMD RCAS
      (`src/up/shaders/rcas.comp`, with noise removal, on the HDR image through a reversible tonemap) after either
      backend.
    - Strength comes from the new "Upscaler Sharpening" slider in Options > Graphics: 0..100 %, steps of 5,
      default 25. 0 skips the pass.
    - The slider is a real CSliderWidget built like the engine's XML menu builder builds one. Its link is renamed, so
      no cvar or Lua is involved.
    - Saved as `sharpness=` in `upscaler_menu.txt`; option `upscale_sharpness` overrides it.
  - **Foliage sway.**
    - Swaying leaves and bushes have no motion vectors, and that is the DLSS foliage shimmer (lab: the per-pixel
      deviation of a bush under DLAA drops to the level of the wall behind it without sway).
    - `src/se4/foliage.cpp` redirects the 6 reads of `sha_iAllowTreeSwaying` to a mod copy. That copy
      mirrors the cvar, or holds `min(cvar, mode)` while an upscaler runs. The cvar is never written.
    - Menu row "Foliage Sway (DLSS / FSR)": Off (default) or Game Setting, saved as `foliage_sway=`; option
      `upscale_foliage_sway` overrides it.
  - **VR menu mode** (default on in VR; off with `vr_upscaler_menu=0` or `single_pass=1`):
    - The Rescaling Filter entries work in VR too.
    - The "Max 3D rendering Mpix" row becomes "VR Render Resolution": 33/50/58/67/75/83/90/100 % of the eye
      resolution per axis. The user's desktop MPix cap no longer reaches the eyes.
    - Below 100 % with DLSS/FSR: phase B at that size. Without them, the engine's own resized canvas and the
      Rescaling Filter.
    - Single-pass stereo only without an upscaler at 100 %. The mod switches `vr_bSinglePrepareRendering` in memory
      at frame start (restored before the engine saves cvars).
    - The backend switch also happens at frame start.
    - Saved as `vr_upscaler=` / `vr_render=`; the desktop keeps `upscaler=`.
  - **New symbols:** `slider_alloc_site`, `slider_ctor`, `widget_add_child`, `widget_set_label`, `widget_set_tooltip`,
    `sha_iAllowTreeSwaying`, all four exes. The multichoice and collection constructors are decoded from
    `gfxopt_init` +0xd0c / +0xda4 (same bytes in all four).
  - **Lab tool:** `tools/lab/flat_stability.sh`. Lab options: `dlss_preset`, `lab_foliage_sway`. Details and numbers:
    R29.
- **HUD inset (R22):** while a flat-HUD element renders in an eye pass, the HUD layout map's safe-area calls are
  redirected so screen-anchored elements lay out inside `hud_inset` % (default 70) of each eye, sizes scaled by
  `hud_inset_scale`, centred on the eye's optical axis; world-projected points are untouched. Lab: health/armour/ammo
  moved in from the image edges.
- **Riding crash guard (R21 s3):** boarding a vehicle drops the player's physics body and the native VR locomotion
  handlers read it unchecked (lab crash at 0x14043aaf5 after MountRide). The vrd tick hook now clears the VR
  game-movement bit (vrd+0x2bc 0x10) while a VR player rides or has no body and restores it afterwards, for every VR
  player. Not yet exercised: the lab could not board a vehicle (06_Countryside vehicles locked; the 06b ATV is
  spawned later by the level script).
- Priority (user, 2026-10-25): finish and test everything in SS4 first, then bring SM to parity.
