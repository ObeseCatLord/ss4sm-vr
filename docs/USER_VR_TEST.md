# Headset test procedure (run when the headset is free)

Prerequisites: Envision/Monado running with the Beyond and Knuckles (or SteamVR), mod installed with
`tools/install.py install <game dir>`. That also creates the VR opt-in `Bin/x64/ss4smvr/enable_vr`; remove the file
to play flat again. A flat player joining VR players uses `--mode desktop`. `tools/install.py uninstall <game dir>`
removes everything the mod added.

Steam launch options (Serious Sam 4 / Siberian Mayhem):
  WINEDLLOVERRIDES="msimg32=n,b" PRESSURE_VESSEL_IMPORT_OPENXR_1_RUNTIMES=1 %command% +gfx_strAPI Vulkan

Checks, in order (log: `<game>/Bin/x64/ss4smvr/ss4smvr.log`):
1. Game starts, `vrdev: COpenXRDevice installed`, `xr: session state 5` (FOCUSED).
2. Menu shows in the headset (mirror quad, or stereo once `stereo: submitting engine-rendered eyes`).
3. `eyes: eye 1/2 image ...` lines appear; `stereo:` lines confirm the projection layer.
4. Head tracking: looking around moves the view without judder; IPD/scale look right.
5. Controllers: A continues screens, sticks move/turn, triggers fire; haptics on firing.
   Index layout: R A or R grip jump, R B use / melee kick (E), R stick click crouch, L A reload, L B menu, L/R trackpad press (hold) that hand's weapon wheel,
   L trackpad tap previous weapon, R trackpad tap dual wielding, L grip gadget in/out, R stick click use,
   L stick click sprint.
   Other profiles: `src/core/vr_controls.hpp`.
6. In a level: the log shows `player_vr: local user psd ... marked as playing in VR` and
   `CPlayerVRData ... (local 1)`. The view follows head position and rotation, and the weapon follows the
   right hand. Turn with the right stick and note whether the view turns once or twice as fast as expected.
7. Weapon wheels (one per hand on Index/WMR): hold the right trackpad and move the right hand towards an item
   to pick the right-hand weapon; hold the left trackpad and move the left hand to pick the left-hand (dual-wield)
   weapon. Both trackpads: both hands at once. Touch: Y opens one wheel for both hands. While a wheel is held,
   right A/B cycle the gadget ring.
7i. Gadgets: the left grip takes the selected gadget into the right hand (again: puts it away, the weapons come
   back). Set it off with its gesture:
   - throwables (Black Hole, Nerve Gas, Decoy, Mini Nuke, SM Artillery/Hoverboard): point the LEFT hand where it
     should land - the native arc and marker start at the gadget and follow that hand - then make a throwing motion
     with the right hand;
   - Companion Drone, Serious Time: a throwing motion;
   - Med Kit: the needle onto the left forearm, or pointed into the body (belly, side, hip);
   - Serious Rage: to the mouth; Extra Life: to the chest.
   Log: `gadgets: <use> -> use`, `gadgets: holder use`, `gadgets: launch`. `gadget_motion=0` makes the trigger set
   off any gadget.
7k. Two guns (dual wield): each gun has its own crosshair where it points, with its own spread; a knife in one hand
   has none. Close one eye, then the other: each crosshair stays on what its gun points at.
7l. Wheel spots: hold a wheel and move the hand far to the left (Menu; higher: Netricsa) or right (Quick load;
   higher: Quick save) past the wheel; the spot lights up; let go to run it. Your weapon should not change.
   The spots' labels, the "GADGETS" title and the gadget names should be readable on the panel and sit at the
   panel's depth (not floating in front of or behind it).
7m. Menus and Netricsa: A selects, B goes back and closes Netricsa, the grips switch Netricsa tabs, the trigger
   clicks what you point at. Button prompts should name your controller's buttons (e.g. "L Grip" for the gadget).
7n. Sirian Artifact (S.A.M. crate): the artefact comes up in your hand and waits; crush it with the trigger, a hard
   squeeze (Index) or the grip's second stage (Steam Frame); the skills screen then opens as usual.
7b. Crosshair and HUD: the game's crosshair should sit where the right-hand gun points, at the depth of what it
   hits (close one eye, then the other: it stays on the target), and its spread ring should match the native
   spread. Health, armour, ammo and the objective show around the view; check they are readable and not too far
   out at the lens edges. Turn the crosshair off with `native_crosshair=0` to compare.
7j. Weapon wheel panel: the wheel opens on a panel in front of the hand whose trackpad opened it, the world stays
   sharp (no blur) and in stereo. Note the weapon's look vs where it shoots (known: the visible gun is pitched up).
7c. Vehicles (ATV): the left stick drives. Squeeze both grips on the handlebars; turning the hands should turn the
   real bars 1:1 and steer the ATV in the same direction. Log: `vehicles: grabbed the steering wheel`.
   On every vehicle (also the ATV, chopper and SM snowmobile, which default to a chase camera) you should see from the
   seat, not from behind the vehicle (`vehicles: riding view kept first person`). Note whether the rider's body and
   arms look right from there.
7d. Body IK (`body_ik=1` in options.txt): in a mirror, third-person view (`plcmdThirdPersonView`) or another
   machine, the player body's arms should reach the hands and weapons. Log: `body: IK handler ... registered`.
7e. Foveation: off by default except on a Steam Frame. Try `foveation=1` (fixed) in options.txt. The centre should
   stay sharp and the periphery should be slightly blurrier, with a higher frame rate in dense scenes. The log
   shows `fov: active` and `fov: scene render pass ...`.
7f. Hands: with a one-handed hold, the left arm follows the left controller and the controller model is hidden.
    The fingers curl with the trigger (index finger) and the touches (thumb). Bring the left hand to the rifle's
    foregrip and squeeze: the rifle should follow the line from the right hand to the left hand. Releasing the
    squeeze returns it to one hand. Report whether the hand looks rotated or offset relative to the controller.
7g. Scopes: take the sniper rifle (SS4/SM), the Devastator or the SM crossbow and raise the scope to your eye. A
    round, magnified view with a reticle should sit at the eyepiece, the rest of the world unzoomed. Report the
    eyepiece position (`scope_eye_up_cm`, `scope_eye_back_cm`) and size (`scope_size_cm`) that feel right.
7h. Menus: point at a menu item with the right controller. It highlights, and the trigger clicks it. The left
    trigger moves the pointer to the left hand.
8. Optional A/B (file `<game>/Bin/x64/ss4smvr/options.txt`, one `key=value` per line): `native_vr_action=1` is the
   engine's own VR mode. It is always on as a multiplayer client.
   - Each player action sends the head and both hands.
   - The look follows the head.
   - The right stick turns through the engine's VR rotation: smooth at `turn_speed` (deg/s, default 150), or snap
     with `snap_turn=1`. The mod sets these itself; no game setting is written.
   - Check that turning feels right (no judder; the hands stay on the controllers), that moving still works, and that
     shots leave the gun.
   - `turn_vignette=1` turns on the engine's comfort vignette during turns. It is unchecked: look for a dark ring
     and no image corruption.
8b. Upscaler A/B (Pompeii, look at foliage and fences while standing still and while turning): `upscaler=0`
   (default, no AA), `upscaler=2` (DLSS/DLAA, NVIDIA) and `upscaler=1` (FSR 3.1). Note which looks best for
   shimmer vs sharpness and the frame rate of each; the default follows your pick.
8c. Desktop upscaler menu (flat, no headset; `install.py install <game dir> --mode desktop`, which writes
   `desktop_upscaler=1`): Options > Graphics > Rescaling Filter > "DLSS" or "FSR 3.1" (DLSS only on NVIDIA). It
   switches at once and is remembered in `ss4smvr/upscaler_menu.txt`; any stock filter or Default turns it off.
   Quality = "Max 3D rendering Mpix" in the same menu: Unlimited (or a cap above your resolution) is native AA
   (DLAA / FSR native), a lower cap renders fewer pixels and upscales. MSAA must be off. Check the image while moving
   and the frame rate at two Mpix settings; log lines `menu: rescaling select` and `upint: flat limiter (menu`.
8d. Upscaler extras (R29), desktop and VR, in Options > Graphics:
   - **"Upscaler Sharpening"** (0..100 %, default 25): sharpening after DLSS / FSR. Look at fine detail and edges.
   - **"Foliage Sway (DLSS / FSR)"** (default Off): stops leaves and bushes from swaying while the upscaler runs, which
     removes their shimmer. Set it to Game Setting and compare a bush.
   - **VR only, "VR Render Resolution"** (the Mpix row in VR): percent of the headset's eye resolution. 100 % with
     DLSS is DLAA; 67 % is DLSS Quality. Check the frame rate and image at 100 / 67 / 50 %.
   - Log lines: `menu: sharpening`, `menu: foliage sway`, `menu: VR render resolution`, `upint: phase B on: DLSS at
     N% of the eye size`, `upint: VR stereo`.
9. Note frame rate (`mirror: N fps`) and any `vrdev: first call slot` lines that are new.
Send the log back; it contains no personal data beyond file paths.
