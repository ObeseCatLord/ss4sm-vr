# Gate R11: loading screen, menus and the 2D layer in VR

Static analysis only (Ghidra project `sam4` copied to `lab/ghidra_r11`, capstone, harvest JSON; no game,
Wine or XR runtime run). Addresses are Sam4.exe 616154 PE VAs unless marked SM (SamSM.exe 628492).
Decompiles: `lab/r11_*.c` (not committed). New read-only tool: `tools/menu_vr_modes.py` (menu vtables and
their VR presentation slots). Member names come from reflection records (`tools/meta_class.py`,
`tools/meta_members.py`); unreflected fields are marked "(unreflected)". Inputs used: `Log/Sam4.log` and
`Bin/x64/ss4smvr/ss4smvr.log` of the failing run (23:36), `tools/lab/level_test.sh`.

Helper semantics used throughout (disassembled): `0x1410c1450` = strcmp (0 if equal), `0x1410c0040` =
case-insensitive equals (1 if equal), `0x1410c0210` = case-insensitive not-equal (1 if different).

## 0. Answers

1. **Loading screen.** It is the menu screen `CMSLoading3` (vtable 0x141b9c3f8). It continues when the
   menu manager's "close all menus" request `0x14005f8e0(mgr)` runs. Four inputs can trigger that request
   (§1.3): menu command 0xb/0xc/0xd/0x21 (Enter, Escape, gamepad accept or back), right-mouse-button up,
   or a click on the CONTINUE button-bar entry (`CMSLoading3::Aux0_OnClick`). Each path is gated by
   `ms_bAllowScreenExit` (+0x17c). The command path is also gated by an unreflected "accepts commands"
   flag (+0x1bc).
   **No check of the VR device lies on the keyboard path** (§1.4). The VR-dependent differences that were
   found are these:
   - The mouse handler 0x140053140 rescales window coordinates into cinema-texture space, while
     immersive mode lays the menus out in the eye drawport. So mouse clicks miss.
   - The engine never turns device buttons 0 and 4 (trigger and A, slot 0xd0) into a menu accept. The
     only menu use of device buttons is the native pointer 0x1401d8430, and it is unusable in SS4 (§2.6),
     plus button 7 as back.
   - The virtual pad was not polled.

   The keyboard failure is not explained statically. §1.5 lists the gate values to log. **Recommended
   fix (§5.1):** after `CMSLoading3`'s own step, do what `CMSLoading3::Aux0_OnClick` does
   (`if (this->ms_bAllowScreenExit) 0x14005f8e0(this->+0xb8)`) on a VR or pad accept. This bypasses every
   input-routing gate. Closing the menu then runs the native start confirmation
   (0x14088fe70 → CGameInfo::gi_gssGameStartStatus = 0).
2. **Immersive or cinema.** `0x1401dcbe0` chooses each frame. The frame is a cinema (2D screen) frame
   when `vr_bForceImmersiveMode == 0`, `viewer+0xd0 == 0` and the top menu's vt+0x1a8 returns nonzero.
   113 of the 128 menu vtables return 1 there, including `CMSLoading3`, and 2 more ask the menu below them.
   Otherwise the frame is immersive.
   With `vr_bForceImmersiveMode = 0` the engine therefore already shows menus and loading screens on a
   world-locked screen, while gameplay with no menu stays immersive (§2). The native pointer cannot work
   in SS4 as shipped (§2.6).
3. **2D layer.** Menus are drawn by `Render` 0x140053b70 → `0x1409500d0(pi+0x200)` (current application
   state's vt+0x38) → menu-state render 0x14094d4f0 → menu manager render 0x1400604f0. That draws into the
   gfx's current drawport (`gfx+0x1c78`) and lays out to its size. In VR it runs inside each per-eye
   `Render(pi, eyeRT, 1)` (0x1401deaf0). The engine already has a "2D only into a texture" variant: the
   fixed-screen branch of 0x1401d96a0. It is the template for a mod quad layer (§3).

## 1. The loading screen

### 1.1 Who opens it, the class, and its slots

- `0x140055ff0(pi)` builds the params `MenuParam.ContinueButton=TRUE` (plus `MenuParam.DesiredTheme/
  LockToStats` when `pi+0x10 → +0x1ec`), then calls `0x14094d9c0(pi, "CMSLoading3", 1, 0.5f, params, 0)`.
  That queues a menu-request application state (vtable 0x141beb9a0, ctor 0x14094c4e0) on `pi+0x200`.
  0x140055ff0 is called by the game loop (0x140058800 at 0x140059bc2, on the loading transition
  `pi+0x3d0 → 1` when `0x1400a8bd0(pi) == 0`) and by 0x140052d60 (pi vt+0x98). The alternative
  `0x140056170` shows only the "QuickLoading" message box.
- `CMSLoading3` (base `CMenuScreen`, size 0x1d0). Reflected members: `ms_pmsGameMode` +0x130,
  `ms_pswProgressBar` +0x138, `ms_abhButtonBar` +0x140, `ms_plwTitle` +0x148, `ms_pwidHint` +0x150,
  `ms_pwidHintHint` +0x158, `ms_piwScanLines` +0x160, `ms_piwHexagons` +0x168,
  `ms_tmServerNotPresentTimeout` +0x170, **`ms_bAllowScreenExit` +0x17c**, `ms_pfrProgressState` +0x188,
  `ms_fProgressPercentage` +0x18c, `ms_tmStartup` +0x190. `ms_pmsGameMode` (+0x130, decompile
  `param_1[0x26]`) is the child screen the step opens (§1.2). Unreflected fields used by the code below:
  - **+0x178 world ready.**
  - **+0x180 start confirmation pending.**
  - +0x184 ready vote.
  - **+0x1bc accepts commands.**
  - +0x1a0/+0x1a8/+0x1b0/+0x1b8 quote list, index, fade time and fade direction.

| CMSLoading3 slot | Sam4 | SM (vt 0x141b80c48) | role |
|---|---|---|---|
| +0x120 | 0x14088f1d0 | 0x14088f540 | init: button bar entry 0 "Loading...", entry 2 "Menu" (F10, cmd 0x20) unless net game, title "CHAPTER %1", quotes |
| +0x138 | 0x14088f0d0 | 0x14088f440 | OnCommand(cmd, arg) |
| +0x158 | 0x14088feb0 | 0x140890220 | OnKey(key, released): 0x53 (F3) toggles ready, 0x5a opens CMSInGameMenu2 if not net |
| +0x168 | 0x14088ff50 | 0x1408902c0 | OnMouse(evt): evt 10 continues, 1/2 cycle quotes |
| +0x190 | 0x140890050 | 0x1408903c0 | per-frame step |
| +0x100 | 0x14088fe70 | 0x1408901e0 | if +0x180: `0x1402048e0(CGameInfo)` = confirm start (gss → 0) |
| +0x110 | 0x140890020 | 0x140890390 | activate: `0x140b71130()->vt[0x548](1)` (meaning unknown) |
| +0x1a8 / +0x1b0 / +0x48 | 1 / 0 / 0 | 1 / 0 / 0 | cinema yes, fixed screen no, monitor-only no (§2.4) |
| script `Aux0_OnClick` (Continue) | 0x14088eba0 | 0x14088ef10 | `if (+0x17c) 0x14005f8e0(+0xb8)` |
| script `Aux2_OnClick` (Menu) | 0x14088ebf0 | 0x14088ef60 | opens CMSInGameMenu2 |
| script `Aux3_OnClick` (Ready) | 0x14088ec50 | 0x14088efc0 | 0x140890c10 ready toggle |

### 1.2 The step 0x140890050: when CONTINUE appears

The step runs every frame (decompile `lab/r11_a.c`).
- While the world loads, `ms_pfrProgressState` (+0x188) goes from 0 to 1/3 (loaded; 3 = net session
  `pi+0xd0->vt[0x60]` true) or 2 (load failed). On 1/3 it sets **+0x178 = 1** unless it is "waiting for
  server to start".
- If `+0x130 == 0`, the top menu is this screen, and a world exists (`0x1400507e0(pi)`), it opens a child on
  top: "CMSStatistics" for SinglePlayer, "CMSGameDetails" for Survival, else "CMSGameOverlay"
  (`0x140060c60(mgr, name, 0, 1, 0)`). It stores `child+0xa0 = this` (the parent link).
- **+0x1bc** = 1 when the top menu (`mgr+0x28`, `0x14005fd70`) is null or this screen. It is also 1 when
  the top menu passes `0x140149540(top, "MSLoading3")` and either the session is a net game with more than
  one player, or the top's class is not CMSGameDetails/CMSPlayerDetails. Otherwise it is 0.
- When +0x178 ≠ 0, the step sets **+0x17c = 1** (exit allowed), then:
  - If `pi+0xd0` is non-null and `pi+0xd0->vt[0x60]() == 0`, it follows `CGameInfo::gi_gssGameStartStatus`
    (+0x454; CGameInfo from `0x14004e3e0(pi)`, reflected; values GSS_NONE 0,
    GSS_WAITING_FOR_PLAYERS_TO_LOAD 1, GSS_WAITING_SINGLE_JOINED_CONTINUE 2, GSS_WAITING_WARMUP_END 3,
    GSS_WAITING_COUNTDOWN 4):
    - 0 → `vt+0x138(this, 0xb, 1)` (auto continue);
    - **2 → `0x14088ee70`** (entry 0 = "Continue", icon `DAT_142275a68`, command 0xb), **+0x180 = 1**;
    - 1/3 → "Waiting for (other) players…", +0x17c = 0;
    - 4 → "Starting in %1";
    - no CGameInfo → +0x17c = 0.
  - Otherwise, if `men_bAutoContinueAfterLoad` (0x14228fd98), `DAT_14228fd9c` (one-shot), `cht_bAutoTestBot`
    (0x142276238) and `0x1400a6000()` (functional test running, `DAT_142274848`) are all 0, it calls
    `0x14088ee70` (show Continue). If any is set, it calls `vt+0x138(this, 0xb, 1)` and clears
    `DAT_14228fd9c`.
- CGameInfo enters state 2 in 0x140203bd0 once all of its players have confirmed the world-loading
  counter (`0x140068750`) and there are fewer than 2.

Whether `pi+0xd0` is non-null in single player is **unknown** (runtime). So the cvar
`men_bAutoContinueAfterLoad` alone may not apply in SP.

### 1.3 Everything that makes Continue happen

All paths end in `0x14005f8e0(mgr)` (SM 0x14005fa60). That function sets `mgr+0x274 = 1` unless
`mgr+0x278` is set. Only the sign-in-change handler 0x14005b2a0 sets +0x278. The menu-manager step
0x140060ec0 (called from menu-state step 0x14094d7e0) then closes every menu (`vt+0xf0`) and clears
+0x274. Closing runs the base `0x140148160`, which calls the top's vt+0x100. For CMSLoading3 that is
0x14088fe70 → `0x1402048e0(CGameInfo)`, which sets gss = 0 when not a net client and `CGameInfo+0x478`
(loading) is 0. The game then starts (confidence M for the order inside the close loop).

| input | route (addresses) | gates |
|---|---|---|
| Keyboard Enter / KP-Enter / Escape | window → pi key handler **0x140053030** (pi vt+0x70; console 0x140aa6870, chat `pi+0x78` 0x14016e150) → current state vt+0x40 = menu state **0x14094cbb0** → `0x140153c20(mgr,key)` → top vt+0x158 (CMSStatistics or CMSLoading3, then focused widget vt+0x60) → `0x140152da0`: key 0x65/0xa5 → cmd 0xb, 0x60 → 0xc → `0x140152a60(pi,cmd)` (swallows 0xb if `pi+0x64 == 0`, the key-protection "unlocked" flag set by 0x14005a7a0) → top vt+0x138 → base 0x1401496c0 forwards to parent `+0xa0` → **0x14088f0d0**: cmd 0xb..0xd/0x21 → `if (+0x1bc && +0x17c) 0x14005f8e0` | (a) 0x14094cbb0: `DAT_14225c394` (initial 1, cleared only by the text-entry menu 0x1408e0c70) ⇒ the menu owner's input device (`0x14005fd80(mgr)` → `0x14004f850` → `0x1401d75f0`) must be named "Keyboard+mouse", or the device-filter stack `DAT_1423ad480/488` must be empty or contain it; (b) a focused widget may consume the key; (c) `pi+0x64`; (d) +0x1bc, +0x17c |
| Gamepad (incl. our XInput pad) | menu-state step 0x14094d7e0 → `0x140aac120` only if `DAT_14239c1c0->vt[0x130]()` ≠ 0 (taken as "window active" from its use with the inactive-FPS logic in 0x140058800; M); sets `pi+0xb0` (0x1400528a0) and, for each MenuCommand binding pressed (`0x1412e4690`), `0x140153ac0(mgr,cmd)` → same command chain | +0x1bc, +0x17c; the pad must be polled |
| Mouse right button up | pi mouse handler **0x140053140** (pi vt+0x78, the mod's `viewport_mouse_event`) → state vt+0x48 = **0x14094ce10** (msg 6 → evt 10) → `0x140153ef0` → top vt+0x168 → 0x14088ff50 `if (+0x17c && evt==10)` | same owner-device gate as keyboard; +0x17c |
| Mouse left click on CONTINUE | same route, evt 5/7 → base 0x140149b60 → widget hit test at the rescaled position → button bar → `CMSLoading3::Aux0_OnClick` 0x14088eba0 (or `CMSStatistics::Aux0_OnClick` 0x1408ddb60 on the child) | +0x17c only, **but the hit position is wrong in VR (§1.4)** |

### 1.4 What a VR device changes (every check examined)

| check | where | effect on the loading screen |
|---|---|---|
| mouse rescale | 0x140053140: with the device global set and `0x140062050(pi) == 0` (top vt+0x48 "monitor-only"), window coords are scaled to (h·viewer+0x54/+0x58, h), h = `CGlobalVRParams::gvrp_pixCinemaScreenResolution` (+0xdc, via `0x14004e1c0(pi)+0x60→+0xd98`) | **Clicks miss in immersive mode.** Menus are laid out in the eye drawport (§3), but the cursor is in cinema-texture space. With viewer+0x54/+0x58 left at the ctor's 1.0/1.0 (0x1401d7d90) the aspect is 1 |
| VR command (button 7) | `0x140b6ade0` callers: 0x140156900 (in-game pause), 0x1401d8430 (fixed screen: close menus), 0x140aac49c (in 0x140aac120: back to CMSInGameMenu2 via 0x14005fc70) | Back only; no accept |
| device slot 0xd0 readers in menu code | 0x1401d8430 / 0x1401dec10 (pointer), 0x1401db670 (laser overlay), **0x14094c1c0** (movie state vt 0x141beb828 step: trigger *held* ends the state), 0x140b6ade0 | **Trigger and A are never mapped to menu accept** |
| `device+0xb3c` → "VRController" | 0x1400528a0, 0x140058800, 0x14040da90 | Not set by the mod (R3). The device is not registered in SS4, so the lookup returns −1 |
| `0x140b6af60` (device+0x08) | 14 callers (0x1400a9c90 … 0x140838496), all in rendering/entity code | Not on the menu/loading path |
| device+0x0c | reader 0x1406db190 | Not on the menu path |
| slot 0x140 IsDashboardVisible | 0x140761380, 0x1407622b0, 0x140762860 (camera calibration) | Not on the menu path |
| `prjIsVRActive` | build constant 0 (R10 §1) | — |
| `0x1401dcbe0` reads `top->vt[0x1a8/0x1b0]` | VR view | Presentation only (§2) |

The keyboard and pad command chain (§1.3) has no VR test. Under VR it differs only in (a) the menu
owner's device and (b) the +0x1bc/+0x17c state, and neither is visible statically. Two further facts:
- The same Enter path works in VR's main menu (lab session).
- In VR the current application state must be the menu state 0x141beb9a0. Only the current state renders
  (`0x1409500d0` → `cur->vt[0x38]`), and the loading screen was visible.

So the movie state 0x141beb828 (whose key handler 0x14094bff0 eats Enter/Escape) was not current.

### 1.5 What to log to close the question (cheap, read-only)

In a detour of the CMSLoading3 step (vt+0x190), once per second log:
- `this+0x178`, `+0x17c`, `+0x180`, `+0x1bc`;
- the top menu's vtable (`*(mgr+0x28)`, mgr = `this+0xb8`) and `this+0x130`;
- `pi+0xd0` and `pi+0x64`;
- the current state's vtable (`**(pi+0x200)`);
- `CGameInfo+0x454` (gss);
- `mgr+0x274/+0x278`;
- `DAT_14225c394` and the owner device name (`0x1412e0c00`).

Also log each call of `0x140153c20` (key, released) and of CMSLoading3 OnCommand (cmd). The first gate
that is 0 when Enter is pressed is the cause.

## 2. Immersive, cinema and fixed-screen presentation

### 2.1 The decision (0x1401dcbe0, `lab/r11_a.c`)

```
viewer+0xd0 ("fixed screen") = top ? top->vt[0x1b0]() : 0     // set: screen 5 m wide at (0, 0.5, -3) in play
                                                              // space, cvars 0x14224cb10/14/18; cleared:
                                                              // 0x1401d8360 resets the laser
if (vr_bForceImmersiveMode==0 && viewer+0xd0==0 && top && top->vt[0x1a8]()) {   // CINEMA
    if (++viewer+0xb0 == 1)  // first cinema frame: place the screen at head yaw (slot 0x80, heading only)
        size = (0x14224cb00=10.0, 10.0/(0x14224cb08/0x14224cb0c = 1920/1080)), pos = head + R(yaw)·(0,0,-0x14224cb04=6.0)
} else { device+0x08 = 1; viewer+0xb0 = 0; }                                      // IMMERSIVE
if (device+0x08 == 0) {                         // cinema
    0x1401d96a0  Render(pi, cinemaTex, 1) into viewer+0x30 (whole frame: world + HUD + menus, mono)
    if (!0x1401da9d0) { 0x1401d9e40(eye 1); 0x1401d9e40(eye 2); }   // quad per eye, world-locked
    viewer+0x78 = 1; 0x1401d8430 (pointer)
} else {                                        // immersive
    device+0x0c = 1
    if (viewer+0xd0) { 0x1401d96a0 (2D state stack only → viewer+0x30); 0x1401d8430 }
    0x1401dc520(viewer, pi, 0)                  // stereo; each eye Render(pi, eyeRT, 1) incl. 2D head-locked
}
if (viewer+0x78 == 0 && viewer+0x20 loaded) 0x1401dd800(viewer, pi, viewer+0x20, 0)  // no player view: world-player stereo
```

The device+0x08/+0x0c meanings are as corrected in R10 (1 = immersive). Nothing in the cinema branch writes
device+0x08: it stays 0 because the base helper 0x140b6a8e0 clears it every simulation tick (R1). The cinema constants
0x14224cb00..cb18 are unnamed .data globals, with initial values 10.0, 6.0, 1920, 1080, 5.0, 0.5 and 3.0.
`vr_fCanvasHeight` (0x142247d88 = −0.55) and `vr_fMenuMovementSpeed` (0x142247d8c = 0.1) are read only by
0x1401dd800. They make the `VRCinema` entity's height follow the head.

### 2.2 Cinema path pieces

- **0x1401d96a0** (cinema texture). It sets `device+0x0c = 0` unless fixed screen. It creates or resizes
  the render-target holder `viewer+0x30` via `0x140c6ca30(holder, gfx, h·w/h, h, …)`, with
  h = `gvrp_pixCinemaScreenResolution`, and gets it via `0x140c6c900`, drawport = `rt+8`. A monitor-only
  top menu (vt+0x48) renders into the desktop drawport instead and shows "This menu screen should only be
  viewed on a monitor!". It then clears and either calls `pi->vt[0x50](pi, texDP, 1)` (cinema), or, in
  fixed-screen mode, copies the drawport and runs only `0x1409500d0(pi+0x200)` (2D state stack).
  `DAT_1422d40c0` (mixed reality) uses eye 3.
- **0x1401da9d0** (VR background world). It needs `vr_bDisableCinemaWorld == 0`, `viewer+0x10` loaded and
  `top->vt[0x1a8]`. It finds the entity named `"VRCinema"` in that world (handle cached at `viewer+0x50`),
  copies its placement and size into viewer+0x54..+0x74, binds the texture `viewer+0x30` to a material
  parameter (`DAT_142277af8`), and renders that world in stereo through 0x1401dd800 → 0x1401dc520.
  - `viewer+0x10` is built by 0x1401de2a0 from `mp_pwldVRBackgroundLevel` (+0x508) of the object returned by
    `0x1401464f0(pi)`, and `viewer+0x28` by 0x1401de380 from `mp_pwldComfortMode` (+0x510) of the object at
    `pi+0x3c8`. Both members belong to the same reflected class, whose name is unknown.
  - Both are called only from 0x1400517e0 (game start), and only if `pi+0x360` already exists. The viewer
    is created lazily by RenderVR (or the mod's `ensureViewer`), so `viewer+0x10` is probably null in
    SS4. Whether SS4 content sets `mp_pwldVRBackgroundLevel` is **unknown**.
  - Without that world, 0x1401da9d0 returns 0 and the quad path below is used.
- **0x1401d9e40** (quad per eye). For each eye it gets the RT (slot 0x28), eye transform (0x78), head
  (0x80) and projection (0x70, `DAT_14224d388/38c`), clears, and draws `viewer+0x30` as a quad of size
  viewer+0x54/+0x58 at the placement viewer+0x5c (quaternion) and +0x6c (position) in play space.
  **This is world-locked, not head-locked.**

### 2.3 Fixed-screen mode (`viewer+0xd0`)

It is chosen when the top menu's vt+0x1b0 returns 1 (16 classes, §2.4). The world stays immersive. The 2D
state stack is rendered once into viewer+0x30, and 0x1401d8430 runs. In immersive mode, nothing in
0x1401dcbe0/0x1401dc520 draws viewer+0x30 as a quad. Each eye's `Render(…,1)` still draws the menus
head-locked. Where (or whether) the fixed-screen texture is shown is **unknown**.

### 2.4 Which menus ask for what (`tools/menu_vr_modes.py`, same counts in SM)

Of 128 CMenuScreen vtables:
- 109: cinema 1, fixed 0, monitor 0. CMSLoading3 is one of them.
- 13: cinema 0, fixed 1. The 11 adjacent vtables 0x141bd1148..0x141bd2318 are built in
  0x1408f76d7..0x1408fa99d, next to the message-box helper 0x1408f4db0; the other two are 0x141bba858
  and 0x141bc6c30.
- 3: cinema 1, fixed 1 (0x1419b4080, 0x141b78988, 0x141b82730).
- 1: monitor-only (0x141a8a1b8, the camera calibration screen, OnCommand 0x140761380).
- 2: delegate to the menu below (vt+0x60 → its 0x1a8/0x1b0; 0x14089a5e0/0x14089a620). These are
  0x141ba2920 and 0x141bbff70. The second one's +0x110/+0x118 (0x1408e0c70/0x1408e0ca0) clear and restore
  `DAT_14225c394` and count `pi+0x29c`, which the game loop treats as "suspend controller input". It is taken
  to be a text-entry screen (inferred, L).

Class names of these vtables are **unknown** (no RTTI; not mapped).

### 2.5 World-locked menus with immersive gameplay

These need no new engine code. Leave **`vr_bForceImmersiveMode = 0`** (0x142277ad0; SM 0x142254bd0).
Then:
- Every frame with a cinema-type menu on top uses the cinema path: start screen, main menu, loading
  screen, statistics, and the in-game pause menu, assuming CMSInGameMenu2 is among the 109.
- Frames with no menu are immersive.
- Fixed-screen menus keep immersive.

To keep forcing immersive only during gameplay, the mod can set it to 1 while `mgr+0x28` is null and to 0
otherwise. That is equivalent to the engine's own rule. The cinema path still submits through device slots
0x28/0x18 for eyes 1/2, so our submit works the same (R10 §5.4).

Costs and caveats:
- The cinema texture is mono and includes the menu's 3D background (Netricsa level).
- The screen is recentred only on the first cinema frame after a non-cinema frame (`viewer+0xb0`).
- Screen size and distance (0x14224cb00/cb04) are unnamed globals. Change them only via a signature
  verified in all four builds (readers at 0x1401dcc13).
- Desktop mouse coordinates already match this texture (§1.4), so mouse clicks work on this path.

### 2.6 The native VR pointer 0x1401d8430

It returns at once in each of these cases:
- the device name (slot 0x110) is "Emulated";
- (no VR background world at `viewer+0x10`, or the top menu is monitor-only) and `viewer+0xd0 == 0`.
  In SS4 this probably always applies on the cinema path (§2.2).

In fixed-screen mode, button 7 closes the menus (`0x14005fd20(mgr, 0)`). Otherwise:
1. **Hand pose.** The pointing hand is toggled at viewer+0xd4 by a trigger press on the other hand. The
   hand pose comes from one of two places:
   - The cinema world's VR camera (`viewer+0xb0 ≠ 0`): `0x1400fa6f0(world)` (+0xe0) → `0x1403cc190` →
     `0x14062cee0(cam, hand)`.
   - Fixed screen only: the local view entity 0x1401d91d0, either a `CVRCameraEntity` (0x14062cee0) or a
     player with vrd (`0x14043b920(player+0x25c8)`, R3 §7.3).

   Optional offsets come from `DAT_142285e70..84` when `DAT_1422aad58/60` are non-empty.
2. **Input-device gate.** `0x14004de60(pi)` (device `pi+0xb0`) must be named **"VRController"**. If not,
   it calls `0x1401d8360` (laser off), sets viewer+0xd8 = 1 and returns. On the first frame after the
   gate passes again, it consumes buttons 0..9 (slot 0xe0) and returns. SS4/SM register no input device of that name (R3 §10), so this gate is never
   passed.
3. **Ray cast.** It intersects the ray with the screen quad (`0x14114d8b0`, quad viewer+0x54..+0x74) and
   gets uv. The position is ((h·w/h)·u, h·v), with h = cinema resolution, so it is in the same space as
   §1.4.
4. **Mouse events.** It calls `pi->vt[0x78]` = 0x140053140(pi, &pos, evt, &size) with these events:
   1 move; 4/3 left down/up from button 0 (`0x1401dec10`: value > 0.5 and not consumed); 7/6 right
   down/up from button 1.
5. **Laser.** It updates the laser length (0x14043ab30/40, 0x14062ce80/b0, 0x1407208f0, 0x140723b20).

For the native pointer to work, the mod would have to provide three things: an input device named
"VRController" as the active device, a VR camera or background world (or a player with vrd plus a
fixed-screen menu), and slot 0xd0 buttons 0/1. **Not recommended.** The mod can replicate step 3 against
its own quad and send the same `0x140053140` events (§5.3).

## 3. Where the 2D layer is drawn

- `Render` 0x140053b70 (pi vt+0x50, SM 0x140053bd0) draws, in this order:
  - player views and HUD: `0x14005be70(dp, pi+0xa8, views, size)`; its result is stored in
    `viewer+0x78` ("a player view was drawn");
  - fade: `0x140053ae0`;
  - **the current application state: `0x1409500d0(pi+0x200)`**;
  - virtual-gamepad overlay;
  - console `0x140aa68a0(pi+0x70)` and chat `0x14016ecc0(pi+0x78)`;
  - game-info overlay `0x140bb1410` (also drawn by 0x140055f10 on the VR desktop mirror).

  A second gfx path (`DAT_14224cd90 && DAT_1422ab2d8`) renders the state stack into another device first.
- Menu state (vt 0x141beb9a0): vt+0x38 = **0x14094d4f0** (SM 0x14094de20) does the fade and calls
  **`0x1400604f0(mgr)`** (SM 0x140060670). That function reads the active drawport `gfx+0x1c78`, recomputes
  the layout when its size changes (cache `mgr+0x298/+0x29c`), processes widgets (`0x140061920`,
  `0x140aad210`, `0x1408a5300`) and draws the screen stack (`0x140061a50` → menu vt+0x198).
- In VR it runs inside every `Render(pi, eyeRT, 1)` issued by 0x1401deaf0 (per eye, from 0x1401dc520).
  That is why menus appear head-locked in both eyes, laid out at 1344×1512. On the cinema path it runs once
  inside `Render(pi, cinemaTex, 1)`. In fixed-screen mode it runs alone into the cinema texture
  (0x1401d96a0).

**Hook point for a mod composition layer.** Keep forced immersive. In each eye's `Render`
(`device+0x1c == 1` and eye `device+0x18` ∈ {1, 2}), skip the menu-state render (detour 0x14094d4f0, or
`0x1409500d0` when its arg is `pi+0x200`). Once per frame, call it yourself into your own drawport, as
0x1401d96a0's fixed-screen branch does with `0x140b57340(&lock, gfx, dp, 2, 0)` … `0x140b585f0`. Submit
that image as an `XrCompositionLayerQuad` or cylinder.

For the mouse, make the texture h = `gvrp_pixCinemaScreenResolution` with the aspect the mod sets in
viewer+0x54/+0x58. Then 0x140053140's rescale matches, or the mod rewrites `pos`/`size` in its existing
`viewport_mouse_event` hook. The HUD is part of the player-view render, not of `pi+0x200`. Where exactly
`0x14005be70` draws it was not traced, so moving the HUD to a layer is **unknown**.

## 4. Ports

| item | Sam4 | Sam4_U | SamSM | SamSM_U | evidence |
|---|---|---|---|---|---|
| men_bAutoContinueAfterLoad | 0x14228fd98 | 0x14228fd98 | 0x14226cf48 | 0x14226bf48 | harvest |
| vr_bForceImmersiveMode | 0x142277ad0 | 0x142277ad0 | 0x142254bd0 | 0x142253bd0 | harvest |
| vr_bDisableCinemaWorld | 0x142277adc | 0x142277adc | 0x142254bdc | 0x142253bdc | harvest |
| CMSLoading3 vtable | 0x141b9c3f8 | — | 0x141b80c48 | — | slot dump (identical layout) |
| CMSLoading3 step (vt+0x190) | 0x140890050 | 0x140892780 | 0x1408903c0 | via vtable | port_list / vt slot |
| CMSLoading3 OnCommand (+0x138) | 0x14088f0d0 | 0x140891800¹ | 0x14088f440 | 0x140891b60¹ | SM: vt slot + disasm (+0x1bc/+0x17c/+0xb8) |
| CMSLoading3::Aux0_OnClick | 0x14088eba0 | — | 0x14088ef10 | — | harvest |
| menu_request_close_all | 0x14005f8e0 | 0x140062230 | 0x14005fa60 | 0x1400623a0¹ | SM: disasm (+0x278/+0x274) and caller Aux0 |
| menu state vt / key / mouse / step / render | 0x141beb9a0 / 0x14094cbb0 / 0x14094ce10 / 0x14094d7e0 / 0x14094d4f0 | — / 0x14094f2e0 / 0x14094f540 / 0x14094ff10 / 0x14094fc20 | 0x141bd0230 / 0x14094d050 / 0x14094d280 / 0x14094e110 / 0x14094de20 | — / — / — / 0x140950830 / 0x140950540 | SM: vtable dump |
| menu manager render | 0x1400604f0 | 0x140062e40 | 0x140060670 | — | SM: call in 0x14094de20 |
| pi key / mouse (vt+0x70/+0x78) | 0x140053030 / 0x140053140 | 0x140055980 / 0x140055a90 | 0x140053090 / 0x1400531a0 | 0x1400559d0 / 0x140055ae0 | pi vtable 0x141582e48 / 0x141564c78; manual.json |
| VR pointer / laser-off | 0x1401d8430 / 0x1401d8360 | 0x1401daaf0 / 0x1401daa20 | 0x1401d7fc0 / 0x1401d7ef0 | 0x1401da670¹ / 0x1401da5a0¹ | SM: called from the SM VR view 0x1401dc7c0 |
| cinema texture / world / quad | 0x1401d96a0 / 0x1401da9d0 / 0x1401d9e40 | 0x1401dbd60 / 0x1401dd090 / 0x1401dc500 | 0x1401d9260 / 0x1401da5b0² / 0x1401d9a00 | 0x1401db910 / — / 0x1401dc0b0 | port_list; SM VR-view calls |

Footnotes:
1. Unique 24-byte signature; the 160-byte confirmation does not match because the function is short or
   differs later. Treat as M confidence until confirmed by a slot or caller.
2. Called from the SM VR view; contains the `"VRCinema"` string chunk 0x1401da61c (exact start M).

Reflected offsets are identical in SM: `ms_bAllowScreenExit` +0x17c, `gi_gssGameStartStatus` +0x454,
`gvrp_pixCinemaScreenResolution` +0xdc, `mp_pwldVRBackgroundLevel` +0x508. SM's OnCommand uses the same
+0x1bc/+0x17c/+0xb8, and its +0x100 the same +0x180.

## 5. Recommended changes

1. **Loading-screen Continue in VR (minimal, input-independent).**
   - Detour the CMSLoading3 step (vt+0x190; take it from the vtable `vt = *(void**)this` checked against
     the per-build vtable above, or sign 0x140890050 / SM 0x1408903c0).
   - After the original returns, if our device is installed, `*(int*)(this+0x17c) != 0`, and an accept
     edge was seen this frame, call `menu_request_close_all(*(void**)(this+0xb8))` (Sam4 0x14005f8e0, SM
     0x14005fa60). Accept edges are: the right or left trigger (device button 0), A (button 4), or the
     virtual pad's A.
   - This is exactly `CMSLoading3::Aux0_OnClick`. It runs on the game thread inside the menu step, and the
     native close then confirms the start (0x14088fe70 → gss 0).
   - Optional: also set `men_bAutoContinueAfterLoad = 1` while VR is on (restored on shutdown like the
     other overrides). It only acts when `pi+0xd0` is null or reports a net game (§1.2).
2. **Instrument before changing more** (§1.5). If the log shows Enter reaching OnCommand with +0x1bc = 0
   or +0x17c = 0, the fix in 1 already covers it. If `0x140153c20` is never called, the cause is upstream:
   window or keyboard focus (`focus.cpp` subclass) or the owner-device gate in 0x14094cbb0 (pad assigned
   to the player). Report that separately.
3. **Menus world-locked.** Stop forcing `vr_bForceImmersiveMode` while a menu is on top: set it to 0 while
   `mgr+0x28 != 0` (mgr = `pi+0xb8`), and 1 otherwise, or simply 0. The engine then draws menus and
   loading screens on its cinema quad (0x1401d9e40). Mouse clicks then land correctly. Lab check: cinema
   frames call slot 0x80 once on entry (placement) and 0x28/0x78/0x80/0x70 per eye from 0x1401d9e40. Later, for a
   comfortable and resizable surface, move to an OpenXR quad layer using the §3 hook (skip the per-eye menu
   render, render the state stack once into our texture). That also removes the mono 3D background.
4. **VR pointer without "VRController".** Each frame, while a menu is on top, the mod should:
   - intersect the aim ray with its own quad (or with the cinema quad: viewer+0x54..+0x74 in play space,
     the same space as slot 0x80);
   - call `0x140053140(pi, &pos, evt, &size)` with `size = (h·aspect, h)`, h = `gvrp_pixCinemaScreenResolution`
     (`*(int*)(*(*(0x14004e1c0(pi)+0x60)+0xd98)+0xdc)`) and `pos` in that space;
   - send evt 1 every frame, 4/3 on trigger down/up and 7/6 on B down/up.

   That is what 0x1401d8430 does. Right-button up (evt 6) also continues the loading screen
   (0x14088ff50). Do not register a "VRController" input device: the game loop would then switch the
   player's input to it (R3).
5. **Gamepad A.** Keep the virtual pad polled (fixed separately). The pad's bound MenuCommands reach
   CMSLoading3 through 0x140aac120 → 0x140153ac0.

## Unknowns
- The actual runtime cause of the keyboard Return failure in VR (§1.4, §1.5).
- Whether `pi+0xd0` is non-null in single player (decides the cvar path versus the gss path in §1.2).
- `vt[0x548]` of the 0x140b71130 object (called with 0/1 when Continue appears or the screen activates).
- Content value of `gvrp_pixCinemaScreenResolution` and whether `mp_pwldVRBackgroundLevel` is set in
  SS4/SM.
- Where the fixed-screen texture is shown in immersive mode (§2.3).
- Class names of the 16 fixed-screen and 2 delegating menu vtables.
- `viewer+0x20` world-player source (rendered by 0x1401dd800 when no player view exists).
- `DAT_142247698` (initial 1; used by the menu manager, CMSLoading3 init and the movie state).
- Where the HUD is drawn inside `0x14005be70`.
