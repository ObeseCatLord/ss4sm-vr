# R33: VR Controls menu (Sam4.exe 616154, SamSM.exe 628492)

Sources: Ghidra decompiles of `vrctl_init`, `options_init`, `settings_init` and the VR Controls screen step (private
scratch copy of `lab/ghidra_r19`), the Fusion 2017 menu as reference, lab runs on the isolated SS4 and SM copies.

## 1. What is left of Fusion's menu

- **The screens are still in both games:**
  - `CMSVRControlsOptions` (init `0x1408ee4b0`, SM `0x1408ee870`);
  - `CMSVRControlsOptionsSimple` (init `0x1408f1590`).
  Only the button that opened them was removed from Options.
- **The init builds Fusion's rows** (cvar links in brackets) into a list under the screen's `MainPanel`:
  - Locomotion (teleport / classic), Strafe Movement, Always Move At Full Speed;
  - teleport speed, Blink Teleport, Step Movement and its speed, Click To Move, Movement Orientation;
  - Comfort Mode (`vr_bEnableComfortLocomotion`), Comfort Radius;
  - Rotation Controller, Rotation Method, Rotation Angle, Rotation Sensitivity, Movement On Rotation Controller;
  - weapon pitch, trigger threshold, vibration, the weapon-selection placement rows;
  - Omni rows, only with an Omni device.
- **Title.** The init picks "VR Controls" or "Advanced VR Controls" (`0x140aa78a0`). In the lab it is "Advanced VR
  Controls".
- **Options init** `0x1408c3c90` (SM `0x1408c3d30`, an alt signature) and the in-game **Settings init** `0x1408d06f0`
  (SM `0x1408d07d0`).
  - A submenu button is: alloc `0x1c8` of the button type (decoded from `48 8B 15 rel32 B9 C8 01 00 00 E8` in
    settings_init), ctor, label, target screen IDENT at button+0x1a0, align(0, 2), tooltip. These are calls
    16 / 18 / 20 / 21 of settings_init.
  - The Unrestricted exes have no `options_init` match, so there only Settings gets the button.

## 2. The mod's menu (`src/se4/gfx_menu.cpp`)

- **Button.** In VR, Options and the in-game Settings get **VR Controls** right after Graphics Options. The button
  opens `CMSVRControlsOptions`, with the tooltip "Turning, comfort, movement and the mod's VR options".
- **Rows the mod answers.** The link is renamed and answered by the `wlink_get` / `wlink_set` / `wlink_reset` /
  `wlink_snapshot` hooks, as in R29. No cvar is written.

  | row | link | mod value |
  |---|---|---|
  | Rotation Method | `ss4smvr_turn_method` | 1 smooth, 2 snap (`turn::setSnap`) |
  | Rotation Controller | `ss4smvr_turn_hand` | 0 none, 1 left, 2 right, 3 both sticks (`turn::feed`) |
  | Rotation Sensitivity | `ss4smvr_turn_speed` | slider units × 9.00009 rad/s = the mod's turn speed (R26 s3.2) |
  | Comfort Mode | `ss4smvr_turn_vignette` | the native comfort mask on turns (R26 s3.4) |
  | Ammo Counters (added row) | `ss4smvr_ammo_counter` | on / off (R32) |

- **Native rows left as they are** (the native code the mod runs reads them):
  - Comfort Radius (`vr_fComfortLocomotionMaskRadius`, the mask);
  - Rotation Angle (`vr_fRotationAngle`, the native snap step).
- **Hidden rows:**
  - teleport and step locomotion, Click To Move, Windows MR input;
  - the weapon-selection placement (no reader in SS4) and weapon pitch;
  - Movement Orientation, Strafe Movement, Always Move At Full Speed, Movement On Rotation Controller. The mod
    moves through the virtual pad, and the native VR movement these rows set gets centred sticks
    (`vr_device.cpp`), so they would do nothing.
- **Row enable.** The screen step enables Rotation Sensitivity (smooth) or Rotation Angle (snap) from
  `vr_iRotationMethod` (`vrmenu_rot_method_read`: `0x1408efcaf`, SM `0x1408f006f`, `8B 0D disp32`). Its disp32 is
  retargeted to the mod's turn method, as R26 does for the turn itself, so the enabled row follows the mod's choice.
- **Saved** in `ss4smvr/upscaler_menu.txt` (`core::formatMenuState`): `turn_method`, `turn_hand`, `turn_speed`
  (deg/s), `turn_vignette`, `ammo_counter`. They are applied before `turn::install` / `ammo::install`, and the
  options.txt `snap_turn`, `turn_speed`, `turn_vignette` and `ammo_counter=0` win.
- **Default (F2) → Yes** sets the mod's defaults: smooth, right stick, 150 °/s, no vignette, counters on.

## 3. Bug found in the lab

- The first version retitled the screen after the init (`screen_set_title`, settings_init call index 2). That
  setter rebuilds the screen's layout, so it dropped every row the init had just made. The screen showed only its
  title, and the walk found 0 links.
- The mod now keeps the init's own title. The symbol stays harvested but is unused.

## 4. Lab (2026-11-09, simulated Monado, pointer and trigger)

- **SS4, main menu → Options.** The **VR Controls** button is row 6, after Graphics Options. The screen shows Comfort
  Mode, Comfort Radius, Rotation Controller (Right), Rotation Method (Continuous), Rotation Angle (greyed), Rotation
  Sensitivity (0.29 = 150 °/s) and Ammo Counters (On). Log: `5 rows answered by the mod, 13 hidden`.
- **Rotation Method → Discrete.**
  - The popup lists Same As Movement / Continuous / Discrete.
  - Choosing Discrete enables Rotation Angle and greys Sensitivity, and logs `turn_method=2`.
  - The file is saved with `turn_method=snap`.
  - After a restart: `turn: native turn patched (snap ...)`.
- **Ammo Counters → Off:** `ammo: counters off`, saved as `ammo_counter=0`.
- **SM, main menu → Options:** the same button and screen. Discrete, then Default → Yes, gives `turn_method=1`,
  `turn_hand=2`, `turn_speed=150`, `turn_vignette=0` and `ammo_counter=1`, with no "Cannot get values" lines.
- **VR suites** (this build): SS4 21 passed, SM 21 passed, 0 failed.
- **Not click-tested:** the in-game Settings button (the same code as Options).
