# Headset test procedure (run when the headset is free)

Prerequisites: Envision/Monado running with the Beyond and Knuckles (or SteamVR), mod installed with
`tools/install.py install <game dir>`, VR opt-in enabled (`touch "<game>/Bin/x64/ss4smvr/enable_vr"`, remove
the file to play flat again).

Steam launch options (Serious Sam 4 / Siberian Mayhem):
  WINEDLLOVERRIDES="msimg32=n,b" PRESSURE_VESSEL_IMPORT_OPENXR_1_RUNTIMES=1 %command% +gfx_strAPI Vulkan

Checks, in order (log: `<game>/Bin/x64/ss4smvr/ss4smvr.log`):
1. Game starts, `vrdev: COpenXRDevice installed`, `xr: session state 5` (FOCUSED).
2. Menu shows in the headset (mirror quad, or stereo once `stereo: submitting engine-rendered eyes`).
3. `eyes: eye 1/2 image ...` lines appear; `stereo:` lines confirm the projection layer.
4. Head tracking: looking around moves the view without judder; IPD/scale look right.
5. Controllers: A continues screens, sticks move/turn, triggers fire; haptics on firing.
   Index layout: R A jump, R B crouch, L A reload, L B menu, L trackpad press weapon wheel (hold),
   R trackpad press dual wielding, L grip gadget quick use, R stick click use, L stick click sprint.
   Other profiles: `src/core/vr_controls.hpp`.
6. In a level: the log shows `player_vr: local user psd ... marked as playing in VR` and
   `CPlayerVRData ... (local 1)`. The view follows head position and rotation, and the weapon follows the
   right hand. Turn with the right stick and note whether the view turns once or twice as fast as expected.
7. Weapon wheel: hold the left trackpad (Index) or Y (Touch). The native wheel opens with slow motion. Move the
   right hand up/down/left/right to highlight, then release to select. The left hand selects the second-hand
   weapon.
7b. Crosshair: the game's crosshair should sit where the right-hand gun points, and its spread ring should
   match the native spread. Turn it off with `native_crosshair=0` in options.txt to compare.
8. Optional A/B (file `<game>/Bin/x64/ss4smvr/options.txt`, one `key=value` per line):
   `native_vr_action=1` sends head and hands with each player action (needed for multiplayer). Turning
   then uses the engine's VR rotation (`vr_iRotationMethod` 1 = smooth, 2 = snap;
   `vr_iRotationController` 2 = right hand). Check that turning and moving still work, and that shots
   leave the gun.
9. Note frame rate (`mirror: N fps`) and any `vrdev: first call slot` lines that are new.
Send the log back; it contains no personal data beyond file paths.
