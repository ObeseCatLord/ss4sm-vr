# Goal: a complete 6DoF VR mod for Serious Sam 4 and Siberian Mayhem

The mod is done when every item below passes in **both** games (Sam4.exe 616154 and SamSM.exe 628492, plus
the Unrestricted builds), on Proton/Monado with the Bigscreen Beyond and Knuckles, on SteamVR, and on native
Windows. Each item names its proof. "Lab" means the simulated Monado on the private display
(`tools/lab/level_test.sh` + `tools/lab/check_log.py`). "Headset" means a run of `docs/USER_VR_TEST.md` by
the user. Status lives in `docs/IMPLEMENTATION_STATUS.md`. This file only changes when the scope changes.

## 1. Rendering
- [ ] Stereo world in both eyes, through the engine's own VR view and the device's per-eye targets. Proof:
      lab captures show a level in stereo (the menu already works, 2026-10-23); headset parallax is correct.
- [ ] Single-pass stereo works and is the fastest path, or the performance matrix shows two-pass is
      better. Proof: `docs/PERF.md`.
- [ ] Head tracking 6DoF at the runtime's rate (90 Hz on the Beyond) with no judder. Proof: headset, and
      frame timing in the log.
- [ ] Menus, loading screens, Netricsa and cutscenes appear on a comfortable world-locked surface, not
      head-locked. Proof: lab captures, headset.
- [ ] HUD readable, with the native crosshair projected along the barrel and depth-correct in each eye.
      Proof: headset.
- [ ] Foveated rendering:
  - **Dynamic (eye-tracked)** foveation on by default when the headset is a Steam Frame (gaze available),
    off otherwise.
  - **Fixed** foveation available as an option on every headset.
  - Implemented as a Vulkan fragment-shading-rate attachment on the scene passes, which works with streamed
    headsets and Valve's foveated streaming.
  - Quad views evaluated (`docs/research/DLSS_FOVEATION.md`). They're deferred unless VRS proves insufficient.
  - Proof: perf matrix, headset, and a Steam Frame check when one is available.
- [ ] Upscaling/AA: FSR 3.1 (vendor-neutral) and DLSS/DLAA (NVIDIA) per eye, with jitter, motion vectors,
      depth and per-eye history. Neither game has TAA or an upscaler. Proof: perf matrix and image check.
- [ ] Performance: holds the headset rate in a dense combat scene at the recommended resolution on the
      RTX 4090. Proof: `docs/PERF.md` (Vulkan vs D3D11 vs D3D12 flat baseline; two-pass vs single-pass;
      render scale; foveation; upscaler).

## 2. Input and locomotion
- [ ] Controllers on Index, Touch, Vive, WMR and simple profiles drive every native command through the
      virtual pad (the game polls it). Proof: lab button presses through the Monado input driver advance
      menus and move, jump, fire and reload in a level.
- [ ] Smooth locomotion (head or hand relative), smooth and snap turning, roomscale movement, recentre.
- [ ] Haptics on both hands from native rumble and VR vibration.

## 3. Weapons
- [ ] Hand-aimed weapons through the engine's CPlayerVRData. Shots leave the barrel. Proof: lab: the
      local vrd exists; the barrel ray hits the crosshair point.
- [ ] Dual wielding with per-hand fire and a crosshair per weapon.
- [ ] The native weapon wheel and gadget ring, selected by hand movement, with native slow motion.
- [ ] Two-hand steadying for two-handed weapons.
- [ ] Picture-in-picture scopes for the sniper rifle and every other usable scope: a magnified view through the
      scope lens of the hand-held weapon, while the world keeps the normal eye FOV. Native zoom state, damage
      and sounds stay native. The full-screen scope overlay is suppressed in VR (as in the SS2 VR mod).

## 4. Melee, gadgets, vehicles
- [ ] Physical melee: inertia (mass-scaled follow), swept collision that stops at walls, and native damage
      above the swing-speed threshold. Validated by the server in multiplayer.
- [ ] Gadgets held in the hand and used immersively: throwables thrown by hand velocity, activated
      gadgets triggered in hand.
- [ ] Vehicles: the thumbstick always drives. The real steering wheel or handlebars can be grabbed and turn
      1:1 with the hands (done in code for the wheeled vehicles; needs a lab or headset check). The mech and
      rideables are covered.

## 5. Body and multiplayer
- [ ] Full-body IK for VR players, visible locally (mirrors, third person, first-person body option) and on
      every other machine.
- [ ] Multiplayer: VR and desktop players together. Desktop players install the mod; guards are on and
      the desktop is otherwise unchanged. VR poses reach the server (ClientActionVR). There is a handshake
      with a version and build check. Proof: two-client lab (VR client + desktop client + server).
- [ ] No crash for any mix of VR and desktop peers. Every engine path that the device or vrd reaches
      unguarded is covered (slot 0x208, 0x1d8, controller placement, ClientActionVR so far).

## 6. Robustness and release
- [ ] Unknown or modified builds are refused (fingerprint); flat play is unchanged without the opt-in.
- [ ] `tools/install.py` adds files only, keeps a receipt and uninstalls cleanly. The user's settings and
      saves are never modified by the mod. Lab runs back saves up and suppress autosaves.
- [ ] `docs/USER_VR_TEST.md` passes on the headset for both games. Native Windows is validated.
- [ ] Host tests pass (`ctest`). Every engine address comes from the generated per-build tables, with gate
      notes for each.
