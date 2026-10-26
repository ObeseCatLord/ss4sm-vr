# Gate R19: button prompts that show the VR controller's buttons (Sam4.exe 616154, SamSM.exe 628492)

Static analysis only. Ghidra copy `lab/ghidra_r19` (of `lab/ghidra_r16`) and `lab/ghidra_r19f` (Fusion, of
`lab/ghidra_main`). Decompiles are in `lab/r19/{a..i,fz}.c`, extracted content in `lab/r19/{ss4,sm,fusion}`
(private). SamSM was checked with `port_list.py`, call graphs (`callers.py`), string anchors, vtable slot dumps
and raw disassembly. Nothing was run. Addresses are Sam4.exe PE VAs unless marked SM. Terms:
- "device": an engine input device (`CInputDevice`-like object). All of them live in one global array
  `g_devices` **0x1423ad3e0**, count **0x1423ad3e8** (SM 0x14238a450 / 0x14238a458). A device is referred to
  by its index (`INDEX` / `int`).
- "markup": the engine's HTML-like text (`<img src="…">`, `<span class="…">`, `<pcw>`), rendered by the
  markup text renderer.
- "CString": the engine string, one pointer wide. Built with `cstring_ctor` (0x1410bfac0, SM 0x1410c0270,
  already in `manual.json`). Released with 0x1410bfce0 (SM 0x1410c0490).
- "ident": an interned name (`ident_ctor` 0x1410c65f0, SM 0x1410c6da0), compared as one 64-bit value.
- "xpad": the XInput gamepad class (vtable 0x142075430, SM 0x14205a2a0). The mod's virtual pad is
  the xpad object for XInput user 0, named "Gamepad0".

## 0. Answers

1. **How prompts pick glyphs.** Every button prompt is text with `{command}` placeholders, for example
   `Hint.HoldToAim=Hold {plcmdAltFire} to aim`, `FireToTryAgain=Press {plcmdFire} to try again` or
   `Hint.CmdFireTake={plcmdUse} Take %1`. They all go through a single chain:
   - `expand_markup` 0x1412e6160 (SM 0x1412e6ad0);
   - `command_markup` 0x1412e6900 (SM 0x1412e7270): looks up the command's binding for one device;
   - `control_markup` 0x1412e0980 (SM 0x1412e12f0): calls that device's **vt+0x38** "control markup";
   - the device's vt+0x38: for gamepads, the icon set's `<img src="XboxButtonA">`.

   The device is the player's last used one (keyboard or "GamepadX"). Menus and widgets force "GamepadX".
   "GamepadX" is an input router. It forwards vt+0x38 to the gamepad that was used last, which in VR is the
   xpad "Gamepad0". The glyph set is per device class, not set by a cvar:
   - xpad: the `icons:` field of the matching line in `Content/Config/Input/GameControllers_PC.txt`
     (the "XInput Controller" line has none), else **"XboxOne"**;
   - DirectInput/HID joystick: that `icons:` field (PS3/PS4), else "Xbox360";
   - Steam Input: the "SteamController" set;
   - touch: the virtual-gamepad images;
   - keyboard: key names inside `<span class="control frame">`;
   - VR controller: the "Xbox360" set or text (§3).

   No `inp_*`/`ctl_*` cvar sets the prompt style. The only related cvar is `inp_bReloadGamepadIcons`
   0x1423ad540 (SM 0x14238a5b0), which reloads the XML icon files. Details in §1–2.
2. **VR prompt remnants.** There are no VR glyph textures in SS4, SM or Fusion. All icon sets are under
   `Content/Shared/Textures/Input/` and none of them is a VR set (§4). What does remain from Fusion:
   - The engine **VR controller input device** `CVRInputDevice`, ident "VRController", vtable **0x142071d68**
     (SM 0x142056ac8). It has 23 controls ("ButtonA" … "LeftGrip" … "RightTrackpad").
   - Its vt+0x38 0x1412edd80 (SM 0x1412ee6e0) returns the **Xbox360** set's glyph when a control's id matches
     one (A/B/X/Y, triggers, stick clicks, Start/Back). Otherwise it returns localized text
     `VRDeviceControlName.*` ("Left Grip", "Trigger", "Right Trackpad"). Fusion's `CVRInputDevice` slot
     0x24d3b90 does the same thing (§5).
   - The bindings file `DefaultVRInputControllerBindings.lua`: ButtonB→plcmdUse, ButtonA→plcmdAltFire.
   - Fusion's 3D controller hints, `vrAddControllerHintLeft/Right` (0x14071fe50 / 0x140720030,
     SM 0x14071f5c0 / 0x14071f7a0), with `ControllerHintLine.pfx` and the Oculus/Vive controller models.

   Fusion VR therefore showed a mix of Xbox360 letter glyphs and plain text, plus optional lines drawn on the
   controller model (TFE/TSE `ControllerHints.lua`).
3. **Cleanest fix.** Detour the xpad vt+0x38 function **0x1412f57d0** (SM **0x1412f61a0**). For XInput user 0
   while VR is active, return a markup string chosen from the current OpenXR profile and the mod's pad→VR
   table (`src/core/vr_controls.hpp`):
   - glyphs the engine already uses (`XboxButtonA/B/X/Y`, `XboxTriggerButtonL/R`,
     `XboxShoulderButtonL/R`, `XboxThumbstickTriggerL/R`, `XboxButtonMenu`, `XboxDPadU/D`) where the letter
     or shape matches;
   - otherwise localized text (`ETRSVRDeviceControlName.*`, which the engine translates for us).

   Add a second, command-level detour on `command_markup` for the two actions that bypass the pad: right B
   (native VR use) → `plcmdUse`, and left grip (gadget) → `plcmdGadgetQuickUse`. Both detours change one
   returned string per call. They write no cvars, no files and no replicated state (§6).

Confidence:
- **H** for the prompt chain, the device dispatch and the icon-set selection: decompiled in Sam4, and the
  same call graph was ported to SM.
- **H** for the absence of VR icon sets (full `.gro` listings of all three games).
- **M** that `<img src>` names from a set *other* than the active one render (for example SteamController
  glyphs while the xpad is active): the image-name resolver was not traced (§7).
- **M** that the "VRController" device is registered at runtime: the service start function exists (§3),
  which contradicts R3 §10, so this is a runtime check.

## 1. The prompt chain

### 1.1 Markup expansion
| step | Sam4 | SM | evidence |
|---|---|---|---|
| `expand_markup(bindings, CString* out, const INDEX* piDevice, const char* text, const char* fmt)`. Scans `{…}`. Each token is split on `,` (alternatives, first non-empty wins) and `:` (`progress` value, as in `{plcmdUse:%1}`). Calls `command_markup` per alternative. A result that does not start with `<img src=` is wrapped in `<span class="control frame"> %1 </span>` (or the `progress` variant), then `fmt` is applied (`<span class="command">%1</span>` from the HUD). If nothing is bound it falls back to the command's localized name (cmd table 0x1412ed2b0 / SM 0x1412edc20, desc+0x10), else `{%1}` | 0x1412e6160 | 0x1412e6ad0 | decompile `a.c`; SM: same 5 callers |
| wrappers: `expand_wrap(bindings, out, piDevice, text)` (fmt = "") and `expand_wrap2(piDevice, bindings, CString* inout)` | 0x1412e6130, 0x1412e6f70 | 0x1412e6aa0, 0x1412e78e0 | `b.c` |
| `command_markup(CString* out, bindings, const INDEX* piDevice, ident idCommand)`. Walks binding table A (`*(b+8)`, +0x10 array of 0x38, +0x18 count: +0 command, +0x20 control code) and table B (+0x20 array of 0x50, +0x28 count: +0x28 control, +0x2c modifier). Control code = `device<<16 | control`. For the first binding whose device == `*piDevice`, out = `control_markup(dev, ctl)`, or `"%1+%2"` with the modifier | 0x1412e6900 | 0x1412e7270 | `b.c`; SM is called from inside expand (0x1412e6c52) and calls control_markup 3× at the same offsets |
| `control_markup(CString* out, const INDEX* piDevice, INDEX iControl)`: `dev = g_devices[*piDevice]`; `dev->vt[0x38](dev, &tmp, iControl)`; if tmp is an `ETRS…` string it is translated (0x1410f7ea0 / 0x1410f8e10, SM 0x1410f87c0 / 0x1410f9720); out = tmp | **0x1412e0980** | **0x1412e12f0** | disasm (r8d passes through), SM call order identical |
| other `control_markup` caller: the binding list of the Customize Controls screen | 0x1408812a0 | 0x1408812f0 | `c.c` |

### 1.2 Who calls it, and with which device
| caller | Sam4 | SM | device |
|---|---|---|---|
| HUD text per player (hints, tips, tutorials, death screen "Press {plcmdFire} to try again", ready/join, spectator, voting) | 0x1400d9bc0 | 0x1400d9ca0 | `profile_device(player)` 0x1401d76e0 (SM 0x1401d7260): `+0x28` if set, else the controller's `pi+0xb0`, else "Keyboard+Mouse" |
| "Hold {%1} to skip" (intro cutscene) | 0x1408d8bb0 | 0x1408d8c20 | as above |
| menu aux-button bar (`CAuxButtonHolder`, `ms_abhButtonBar`: Netricsa, loading, demo…) shortcut = `{cmd}` | 0x1409052d0 | 0x140905840 | player device if it has the gamepad flag (0x1412e0b20 & 2), else **"GamepadX"** |
| widget command labels | 0x140917200 | 0x1409178f0 | "GamepadX" or "Keyboard+Mouse" |
| profile/menu gamepad device | 0x1401d7640 | 0x1401d71c0 | maps any "GenericGamepad" device to "GamepadX" |

`device_switch` 0x1400528a0 (SM 0x140052900) sets `pi+0xb0` to the last used device from the bindings
(0x1412e39b0 / SM 0x1412e4330). If the VR device has `+0xb3c` set, it switches to the device named
"VRController" instead (R3 §10). The mod never sets `+0xb3c`, so in VR the device stays "GamepadX" after any
pad input.

### 1.3 "GamepadX" router
- Created by 0x1401612a0 (SM 0x140161f20): an InputRouter (vtable 0x142071830, SM 0x142056590) named
  "GamepadX", of type "GenericGamepad". It routes every "GenericGamepad" and "JoystickInputDevice".
  `DefaultGamepadBindings.lua` binds to "GamepadX".
- Router vt+0x38 0x1412ed8d0 (SM 0x1412ee230) forwards to `routed[+0x18]`, the last device with input (set
  by vt+0x88 0x1412ed930), else to `routed[0]`. In VR that is the xpad Gamepad0.

## 2. Device classes and glyph sets

### 2.1 vt+0x38 per class (`CString* vt38(this, CString* out, INDEX iControl)`, returns out)
| class | vtable (Sam4 / SM) | vt+0x38 (Sam4 / SM) | result |
|---|---|---|---|
| **xpad** ("Gamepad%1", XInput user at `+0x20`, ctor 0x1412f53b0 / SM 0x1412f5d80) | 0x142075430 / 0x14205a2a0 | **0x1412f57d0 / 0x1412f61a0** | icon set = ident at `+0xa8`. Finds the icon whose id == `xpad_ids[iControl]` (0x1423adae0 / SM 0x14238ab60) and returns its markup `<img src="Name">` (entry +0x20). With no match it returns the id text ("ButtonA"), with no set it returns vt+0x30 |
| DirectInput/HID joystick ("Joystick%1") | 0x142075630 / 0x14205a498 | 0x1412f6740 / 0x1412f7130 | same logic, set at `+0x90`, ids at 0x1423adbf0 |
| Steam Input | 0x14208d768 / 0x1420725c8 | 0x14134a960 / 0x14134b740 | ISteamController action origin → `button_a`…`grip_l`…`pad_r_click`…`trigger_r_pull` of the "SteamController" set |
| touch virtual pad | 0x142070458 / 0x1420551b8 | 0x1412eafd0 / 0x1412eb940 | fixed `<img src="VirtualGamepadButtonA">` …, text for axes |
| **VR controller** ("VRController") | 0x142071d68 / 0x142056ac8 | 0x1412edd80 / 0x1412ee6e0 | see §3 |
| keyboard/mouse | not traced | | key names (`KbdMInput.*`), `<img src="MouseButtonLeft">` |

xpad control indices (static init 0x140033470, 24 controls, vt+0x28 = 0x18):

| idx | id | idx | id | idx | id |
|---|---|---|---|---|---|
| 0 | ButtonUp | 8 | LeftShoulder | 16–19 | LX+ LX- LY+ LY- |
| 1 | ButtonDown | 9 | RightShoulder | 20–23 | RX+ RX- RY+ RY- |
| 2 | ButtonLeft | 10 | ButtonA | | |
| 3 | ButtonRight | 11 | ButtonB | | |
| 4 | ButtonStart | 12 | ButtonX | | |
| 5 | ButtonBack | 13 | ButtonY | | |
| 6 | LeftThumb | 14 | LeftTrigger | | |
| 7 | RightThumb | 15 | RightTrigger | | |

### 2.2 Choosing the icon set
- `xpad_reload_icons` 0x1412f5a10 (SM 0x1412f6400). It finds the controller config by GUID "xinput"
  (0x1412f3ff0) and sets `+0xa8` from:
  1. the static CString 0x1423adad0 (SM 0x14238ab50), which is **not a cvar** and is written by nobody
     (only its init and dtor reference it);
  2. else the config's `icons:` field;
  3. else **"XboxOne"**.

  It logs "Missing gamepad config for XInput device" when the GUID is not found.
- Joystick connect 0x1412f6d40 (SM 0x1412f7750): the PIDVID config `icons:` field, else "Xbox360".
- `inpChangeGamepadIcons()` 0x1412f5320 (SM 0x1412f5cf0) is a script/console function with no
  arguments. It re-runs `xpad_reload_icons` on every "GenericGamepad" device.
- Icon XML loader 0x1412e83b0 (SM 0x1412e8d30) reads `psp_arfpGamepadIconConfigFiles` from
  `Databases/ProjectStartupParams.rsc` (all 9 files, the same in SS4 and SM). Each `<icons name>` set is
  0x68 bytes (+0 name ident, +0x48 icons, +0x50 count). Each `<icon>` is 0x58 bytes:
  - +0 id ident ("ButtonA");
  - +8 name ident ("XboxButtonA");
  - +0x10/+0x18 the TFNM texture;
  - +0x20 the markup CString `<img src="XboxButtonA">`;
  - then controlpos / linestart / lineend / box for the controller diagram.

  The set container is at 0x1423ad548 (SM 0x14238a5b8). `iconset_find(ident*)` is 0x1412e9f90
  (SM 0x1412ea910). An unknown set name logs "\"%1\" icons not found. Gamepads using those icons will be
  disabled.".
- Translation keys with `.PS`/`.Xbox`/`.Stadia` suffixes (Menu.ButtonLayout.Xbox …) are platform variants of
  menu labels. They do not select glyphs (selector not traced, PC uses the plain key).
- `prj_iGamepadButtonLayout` 0x142247244 (SM 0x142228144) / `GetGamepadButtonLayout` choose the
  **bindings** (GBL_*), not the glyphs. Prompts follow the user's real bindings automatically.

## 3. The engine VR controller input device (Fusion leftover)
| item | Sam4 | SM |
|---|---|---|
| service `AppInputDeviceVRController` (group "Game") start / stop | 0x140161490 / 0x1401614e0 (registered at 0x1400071b0) | 0x140162110 |
| ctor (`vt = 0x142071d68`, size 0x18), then register 0x1412e1070 (SM 0x1412e19e0) | 0x1412edc40 | 0x1412ee5a0 |
| name / type ident "VRController" (vt+0x10/+0x18/+0x20) | 0x1423ad6b0 | 0x14238a720 |
| control ids (23 used: ButtonStart, ButtonBack, LeftThumb, RightThumb, LeftGrip, RightGrip, Grip, Trigger, ButtonA/B/X/Y, LeftTrigger, RightTrigger, LeftTrackpad, RightTrackpad, LX±, LY±, RX±) | 0x1423ad6c0 (init 0x140031bc0) | 0x14238a730 (init 0x140031bd0) |
| localized names `ETRSVRDeviceControlName.*` | 0x1423ad780 (init 0x140031e40) | 0x14238a7f0 (init 0x140031e50) |
| vt+0x38: "Xbox360" set icon with id == control id, else the localized name | 0x1412edd80 | 0x1412ee6e0 |

Result per VR control:
- glyph from the Xbox360 set: ButtonA/B/X/Y (XBoxButtonA…), LeftTrigger/RightTrigger (LT/RT),
  LeftThumb/RightThumb (stick), ButtonStart/ButtonBack;
- text: Grip, LeftGrip, RightGrip, Trigger, LeftTrackpad, RightTrackpad ("Left Grip" …).

R3 §10 says no "VRController" device is registered. The service start above registers one. Whether the
"Game" service group is started in SS4/SM was not traced. Either way, making it the prompt device is the wrong
tool:
- switching `pi+0xb0` to it drops the virtual pad (R3 §10);
- its bindings file maps only two commands, so every other prompt would fall back to command names.

## 4. Content inventory (`.gro` listings)

SS4, `Content/SeriousSam4/*.gro` (SM `Content/SeriousSamSM/*.gro` has the same `Shared` paths):
- `Content/Shared/Textures/Input/{PS3Controller,PS3ControllerAsian,PS4Controller,PS5Controller,SteamController,
  XBox360Controller,XboxOneController,XboxSeriesController,NXController,StadiaController}.xml` plus a texture
  folder each (202 entries). No VR, Vive, Touch, Knuckles or WMR set.
- SteamController set (40 textures): `button_a/b/x/y`, `grip_l/r`, `pad_l/r`, `pad_l/r_click`,
  `pad_l/r_touch`, `pad_*_dpad_n/s/e/w`, `pad_*_swipe`, `shoulder_l/r`, `stick`, `stick_click`, `stick_dpad_*`,
  `stick_move`, `trigger_l/r_pull`, `trigger_l/r_click`, `button_start/select`, `gyro`. Names in markup are
  `SteamControllerGripL`, `SteamControllerPadRClick`, `SteamControllerTriggerRPull` …
- XboxOne set names: `XboxButtonA/B/X/Y`, `XboxTriggerButtonL/R`, `XboxShoulderButtonL/R`,
  `XboxThumbstickTriggerL/R` (stick click), `XboxThumbstickTrigger{L,R}{U,D,L,R}`, `XboxDPadU/D/L/R`,
  `XboxButtonMenu`, `XboxButtonView`.
- VR leftovers:
  - `Content/SeriousSam4/Databases/DefaultVRInputControllerBindings.lua`;
  - `Content/SeriousSam4/Presets/Effects/Particles/ControllerHintLine.pfx` (`gvrp_pefControllerHintLine` in
    `GlobalVRParameters.rsc`);
  - `Content/Shared/Models/Objects/Controllers/{OculusL,OculusR,ViveWand}{,Ghost}.mdl` (+ .bmf, textures);
  - `Content/SeriousSamFusion/Textures/Menu/VRfriendly.tex`;
  - `Content/Shared/Databases/VRControllerPointerBeamParams.rsc`.
- Strings in `translation_All.txt`:
  - `VRDeviceControlName.*` (24);
  - `Hint.HoldToResetVR=Hold button to reset` (its VR branch is code chunk 0x14078ad08; the condition was
    not traced);
  - `VR.ControllerActivationHint`;
  - `Menu.VRStepMovementTooltip`.

  42 `{plcmd…}` prompt strings in SM, the same keys as in SS4.
- Fusion 2017 has the same icon XMLs (no VR set). It also has `WindowsMR_L/R{,_Ghost}.mdl`, which SS4 lacks,
  and `Content/SeriousSamHD/Scripts/ControllerHints.lua`. That script branches on `vrGetDeviceName()`
  ("Rift", "WindowsMR", else Vive) and calls
  `vrAddControllerHintLeft/Right(player, "<attachment>", "TTRS:…", dx, dy, len)`. The extracted SS4 Lua subset in `lab/lua`
  never calls these. A full scan of world scripts was not done.

## 5. Fusion 2017 comparison (`Sam2017_VR.jotn-vr-stock`, Linux ELF)
- `CVRInputDevice` vtable 0x312d2d0 (Itanium, two dtor slots, so Fusion +0x40 = MSVC +0x38). Its +0x40,
  0x24d3b90, is the SS4 0x1412edd80 logic line for line:
  - `iconset_find("Xbox360")` (0x24cf010);
  - match on the control id table 0x43b1520;
  - else `VRDeviceControlName` (0x43b1440).

  Count = 0x17. The "Xbox360" string is at VA 0x312ce00.
- Fusion VR's prompt device was this "VRController" device, chosen through device+0xb3c, so `{plcmdUse}`
  showed Xbox360 **B** for the Vive/Touch B binding, and text for grips and trackpads. Fusion also has the
  same `<span class="control frame">`, `VirtualGamepad*` and `MouseButton*` markup strings.
- Conclusion: Croteam never had VR glyph art. Their VR prompts were Xbox letters where the letter existed,
  text otherwise, plus optional 3D hint lines on the controller model in TFE/TSE.

## 6. Mapping the mod's virtual pad back to VR controls

From `src/core/vr_controls.hpp` (`Mapper::update`). "—" means the profile never produces that pad control.
Axes 16–23 keep the engine glyph (they are the sticks or trackpads on every profile).

| pad idx | Index | Touch | Steam Frame | Vive | WMR |
|---|---|---|---|---|---|
| 15 RT | R trigger | R trigger | R trigger | R trigger | R trigger |
| 14 LT | L trigger | L trigger | L trigger | L trigger | L trigger |
| 10 A | R A (or R grip) | A (or R grip) | R A (or R grip) | R trackpad click | R menu |
| 11 B | R stick click | R stick click | R stick click | R grip | R grip |
| 12 X | L A | X | R X | — | — |
| 13 Y (gadget on GBL_STANDARD) | L grip¹ | L grip¹ | L grip¹ | L grip¹ | L grip¹ |
| 4 Start | L B / either menu | L menu | R menu / L View | L menu | L menu |
| 6 LeftThumb | L stick click | L stick click | L stick click | — | L stick click |
| 7 RightThumb (plcmdUse) | R B² | B² | R B² | R menu | R stick click |
| 8 LB (wheel) | L trackpad hold (R: right wheel) | Y | L bumper hold | L trackpad click | L trackpad hold |
| 9 RB (dual wield) | R trackpad tap | — | R bumper tap | — | R trackpad tap |
| 0/1 Up/Down | R A / R B while wheel held | same | L D-pad up/down (R A/B in wheel) | — | — |
| 2 Left (Netricsa), 3 Right (way to go), 5 Back | — | — | — | — | — |

¹ The gadget does not go through the pad (`gadgets.cpp` issues the HUD command). ² Use/melee is the native VR
use path (`kick`). The pad never presses RightThumb on these profiles, so the slot can safely show the VR use
button. Footnotes 1 and 2 hold only for `GBL_STANDARD`. SM's `GBL_CUSTOM` and other layouts move commands, so
the command-level override in §7 step 3 covers them.

Gap: on every profile, `plcmdDisplayNetricsa` (pad D-pad Left), `plcmdShowWayToGo` (Right) and
`plcmdQuickSaveTogglePlayerList` (Back) cannot be pressed. "Press {plcmdDisplayNetricsa} to invoke Netricsa"
then has no truthful glyph. See the recommended implementation, step 4.

Glyph choices that reuse art the active set already loads (safe): Touch/Frame A/B/X/Y → `XboxButtonA/B/X/Y`
(same letters), triggers → `XboxTriggerButtonL/R`, Frame bumpers → `XboxShoulderButtonL/R`, stick clicks →
`XboxThumbstickTriggerL/R`, menu → `XboxButtonMenu`, View → `XboxButtonView`, Frame D-pad → `XboxDPadU/D`.

Index has an A and a B on both hands, so use text there. So do grips and trackpads. The text can be the
engine's own localized strings, `ETRSVRDeviceControlName.RightGrip=Right Grip`, `…LeftTrackpad=…`,
`…ButtonA=Button A`, with the hand from `ETRSMenu.VRControllerLeft=Left` / `…Right=Right`.
`control_markup` translates any `ETRS…` result itself. Plain text is framed by `expand_markup`
(`control frame`).

Optional SteamController glyphs (`SteamControllerGripL/R`, `SteamControllerPadLClick/RClick`,
`SteamControllerTriggerLPull/RPull`, `SteamControllerStickClick`) are the closest art for grips and trackpads.
Use them only after runtime check 2.

## 7. Unknowns and runtime checks
1. **Image-name resolver.** The markup renderer's lookup of `<img src="Name">` was not traced. The parser
   (0x140f6dc50) stores only the name, and no global name→icon search was found near the icon module
   (0x1412e7000–0x1412eb000). The engine itself emits names from fixed sets regardless of the active pad (the VR
   device always uses Xbox360 names, Steam Input always SteamController names). That suggests a global
   lookup, but unproven.
2. **Check:** in the lab, return `<img src="SteamControllerGripL">` for pad idx 13 and look at a gadget hint.
   If it renders as an empty box or as nothing, use text for grips and trackpads.
3. **Caching.** Do HUD hints and aux-button bars re-expand every frame, or cache the string?
   `CGamepadIconsEvent` and the `inp_bReloadGamepadIcons` reload hint at refresh events. Check that a profile
   change mid-game updates an open hint.
4. Is the "VRController" device registered at runtime (§3)? Log `find_device("VRController")`
   (0x1412e0aa0, SM 0x1412e1410) once.
5. Plain-text prompts are not markup and cannot be fixed by the hooks: "Press Up/Down to preselect gadget"
   (`HUD.WeaponWheel.GamepadGadgetTip`), and "Press Back to cancel" in split-screen binding.
6. Whether keyboard use in VR flips prompts to keys (expected: yes, through `pi+0xb0`, and that is correct).

## 8. Ports summary
| symbol | Sam4 | SM | how |
|---|---|---|---|
| expand_markup / expand_wrap / expand_wrap2 | 0x1412e6160 / 0x1412e6130 / 0x1412e6f70 | 0x1412e6ad0 / 0x1412e6aa0 / 0x1412e78e0 | sig 24, call graph |
| command_markup | 0x1412e6900 | 0x1412e7270 | sig 24 (longer: no match), single call from expand, 3 control_markup calls at the same offsets |
| control_markup | 0x1412e0980 | 0x1412e12f0 | sig 24 + identical call sequence |
| xpad vt+0x38 / reload_icons / ctor / vtable | 0x1412f57d0 / 0x1412f5a10 / 0x1412f53b0 / 0x142075430 | 0x1412f61a0 / 0x1412f6400 / 0x1412f5d80 / 0x14205a2a0 | sig 24, `vtable_slot of`, `+0x20` user and `+0xa8` set checked in SM disasm |
| xpad control ids / icon override CString | 0x1423adae0 / 0x1423adad0 | 0x14238ab60 / 0x14238ab50 | SM disasm of the ported functions |
| router vtable / vt+0x38 / GamepadX create | 0x142071830 / 0x1412ed8d0 / 0x1401612a0 | 0x142056590 / 0x1412ee230 / 0x140161f20 | sig, vtable |
| g_devices / count / find_device / register | 0x1423ad3e0 / 0x1423ad3e8 / 0x1412e0aa0 / 0x1412e1070 | 0x14238a450 / 0x14238a458 / 0x1412e1410 / 0x1412e19e0 | disasm, devswitch call |
| icon sets / iconset_find / find icon by id / XML loader | 0x1423ad548 / 0x1412e9f90 / 0x1412e9f30 / 0x1412e83b0 | 0x14238a5b8 / 0x1412ea910 / 0x1412ea8b0 / 0x1412e8d30 | sig, 9 callers match |
| VR device vtable / vt+0x38 / service start / ctor | 0x142071d68 / 0x1412edd80 / 0x140161490 / 0x1412edc40 | 0x142056ac8 / 0x1412ee6e0 / 0x140162110 / 0x1412ee5a0 | "Xbox360" ref, vtable |
| joystick vt+0x38 / Steam vt+0x38 / touch vt+0x38 | 0x1412f6740 / 0x14134a960 / 0x1412eafd0 | 0x1412f7130 / 0x14134b740 / 0x1412eb940 | sig / string |
| translate / is_translatable / cstring dtor | 0x1410f8e10 / 0x1410f7ea0 / 0x1410bfce0 | 0x1410f9720 / 0x1410f87c0 / 0x1410c0490 | sig, call order in control_markup |
| inpChangeGamepadIcons / inp_bReloadGamepadIcons | 0x1412f5320 / 0x1423ad540 | 0x1412f5cf0 / 0x14238a5b0 | harvest |
| vrAddControllerHintLeft / Right | 0x14071fe50 / 0x140720030 | 0x14071f5c0 / 0x14071f7a0 | harvest |
| profile_device / HUD translate / aux-button shortcut / intro skip / widget label | 0x1401d76e0 / 0x1400d9bc0 / 0x1409052d0 / 0x1408d8bb0 / 0x140917200 | 0x1401d7260 / 0x1400d9ca0 / 0x140905840 / 0x1408d8c20 / 0x1409178f0 | sig / string |

The Unrestricted exes were not ported here. Use `port_list.py` from Sam4 for each before enabling.

## Recommended implementation

1. **Symbols** (`manual.json`/`signatures.json`, all four builds where they port):
   - `xpad_control_markup` (xpad vt+0x38), `command_markup`, `xpad_vtable`, `router_vtable`;
   - `input_devices` + `input_device_count`;
   - `translate`;
   - `cstring_ctor` and `ident_ctor` are already present.

   Prefer the vtable-slot read (`*(void**)(xpad_vtable+0x38)`) to a fresh signature for the hook target. The
   signature `port_list` found for command_markup only matched at 24 bytes.
2. **Hook A: control level** (new `src/se4/prompts.cpp`, shared code). MinHook on `xpad_control_markup`:
   ```
   CString* __fastcall hk(void* self, CString* out, int ctl) {
       if (!vrActive() || *(int*)((char*)self + 0x20) != 0) return real(self, out, ctl);  // virtual pad = XInput user 0
       const char* m = core::prompts::markup(kind, ctl);   // nullptr = keep the engine glyph
       if (!m) return real(self, out, ctl);
       cstring_ctor(out, m);                               // out is uninitialised caller storage
       return out;
   }
   ```
   `kind` comes from the same place as `gamepad.cpp` (the right hand's OpenXR profile, else the left). The
   table is pure data in `src/core/` next to `vr_controls.hpp`, so it can be unit-tested and kept in step with
   the `Mapper`:
   - glyph strings (`<img src="XboxButtonA">`) where §6 says the art matches;
   - `ETRSVRDeviceControlName.*` text elsewhere (translated by `control_markup`);
   - hand-qualified English only where no key exists (for example "L A").

   This covers HUD hints, tutorials, death and respawn prompts, voting and spectator lines, the intro skip,
   Netricsa and loading-screen button bars, and widget labels. All of them go through "GamepadX", then the
   router, then the xpad.
3. **Hook B: command level.** MinHook on `command_markup(CString* out, void* b, const int* piDev,
   uint64_t idCmd)`. When VR is active and `g_devices[*piDev]`'s vtable is the router or xpad vtable:
   - `idCmd == ident("plcmdUse")` on Index/Touch/Frame → the VR use button (R B / B);
   - `idCmd == ident("plcmdGadgetQuickUse")` → L grip, on every profile.

   Build the text with `translate()`, because `expand_markup` does not translate. Return a glyph or plain text
   (`expand_markup` frames it). For anything else, call through. This keeps those two prompts right under any
   saved pad layout (SM `GBL_CUSTOM`). Cache the two idents once, from `ident_ctor`.
4. **Unreachable commands** (Netricsa, way-to-go, player list). Either give them a VR binding in
   `vr_controls.hpp` (then add rows to the table), or let Hook A return an empty string for pad idx 2/3/5.
   `expand_markup` then falls back to the command's localized name ("Netricsa"). Prefer the binding: the hint
   "Press {plcmdDisplayNetricsa} to invoke Netricsa" is shown to every new player.
5. **Do not:**
   - write `0x1423adad0` or call `inpChangeGamepadIcons`: that swaps whole sets by control id, and the
     SteamController ids ("button_a") do not match xpad ids;
   - route prompts through the engine "VRController" device (§3);
   - write any cvar or binding file.

   Both hooks only change returned strings on the simulation and UI thread that already calls them. No
   replication is involved: other players' prompts use their own devices.
6. **Optional:**
   - SteamController glyphs for grips and trackpads, behind option `prompt_steam_glyphs` after runtime
     check 2;
   - 3D hint lines on the hand models through `vrAddControllerHintLeft/Right` (needs attachment names on
     the mod's hand or controller models, not researched; skip).
7. **Lab checks** (lab Monado, all five profiles via the simulated interaction profiles):
   1. Death screen "Press [RT] to try again" shows the trigger glyph.
   2. A use hint shows R B on Index/Touch/Frame and R menu on Vive.
   3. A gadget hint shows L grip.
   4. Netricsa's button bar shows VR labels.
   5. Desktop (VR off) prompts are unchanged.
   6. A real second XInput pad (user 1) keeps Xbox glyphs.
   7. The Customize Controls screen (it also calls `control_markup`) is acceptable with VR labels, or filter it
      by return address (the call at 0x14088130b / SM 0x14088135b, so return address +5).
