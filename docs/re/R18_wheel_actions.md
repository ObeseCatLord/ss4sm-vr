# Gate R18: Netricsa / menu / quick save / quick load spots on the hand wheel (Sam4.exe 616154, SamSM.exe 628492)

Static analysis only. Nothing was run. Ghidra copies of `lab/ghidra/fusion` and `lab/ghidra_r16` (scratch, not kept).
SamSM was checked with `port_list.py`, `port_sites.py`, `sig_at.py`, vtable slot reads and raw disassembly.
Addresses are Sam4.exe PE VAs unless marked SM. Fusion addresses are ELF VAs of `Sam2017_VR.jotn-vr-stock` ("Fusion").
Terms follow R16:
- "wheel": the desktop `CWeaponSelectionWheelHudElement`.
- "vrwheel": the Fusion VR wheel object (`CVRWeaponSelection*`, no RTTI name in SS4). It lives at
  **CPlayerController+0x1c8** in both SS4 builds (R3).
- "pi": the project instance (`0x14024c710(entity)` returns it, SM 0x14024c330).
- "player": `CPlayerActorPuppetEntity`.

Member names come from reflection (`meta_class.py`, `meta_offset.py`). Fusion layout numbers come from the decompiled
constructors and from the text resource `Content/SeriousSam3/Databases/WeaponWheelSelectionInfo.rsc` (MetaTextFile). That
resource was read from `70_440323_10_Game_SeriousSam3_edit.gro` into scratch and is not in the repo.

## 0. Answers

1. **Fusion design.**
   - The VR wheel is a 3D model ring centred on the controller (not beside it). The action buttons sit to the
     **left and right of the outer ring**.
   - Left column: Menu at the ring's height, and Netricsa 15 cm above it.
   - Right column: Quickload at the ring's height, and Quicksave 15 cm above it.
   - Each hand points with its controller's −Z ray. A button is chosen only on a direct ray hit. A weapon slot also
     falls back to the slot nearest the ray.
   - The action runs **on release** of that hand's wheel button. The button's slot `+0x50(hand)` first commits that
     hand's weapon pick, then performs its action (§1).
2. **SS4/SM still contain the Fusion buttons.**
   - The VR wheel's button builder, the Menu / Netricsa / QuickSave(load) button classes and their action and
     availability functions are compiled into both builds.
   - These native functions are "what the Fusion VR button ends up in":
     - Menu `0x140451450` (SM 0x140451580);
     - Netricsa `0x1404514e0` (SM 0x140451610);
     - QuickSave/QuickLoad `0x1404517c0` (SM 0x1404518f0).
   - Their availability gates are `0x140453f90` / `0x140453fe0` / `0x1404540c0` (SM 0x1404540f0 / 0x140454140 /
     0x140454220).
   - Each action reads only `button+0x08` (vrwheel pointer) and, for save/load, `button+0x40` (1 = save). The mod can
     call them with a stack stub (§2.1).
   - Desktop equivalents (§2.2–2.5):
     - Netricsa: `plcmdDisplayNetricsa` in controller vt+0x18 (same field writes as the VR button).
     - Menu: `syscmdInvokeMenu` / pad Start → `0x14094d9c0(pi, "" or "CMSInGameMenu2", 1, 0.5f, …)`.
     - Quick save: `syscmdQuickSave` / pad Back (`plcmdQuickSaveTogglePlayerList`) → `0x14094bc90`.
     - Quick load: `syscmdQuickLoad` → `0x1404187c0` → `0x1401ccd90`. The VR wheel's own quick load is the deferred
       flag `vrwheel+0x58`, consumed by `0x140454370` at the end of the controller step.
3. **Thread.**
   - Every native above runs inside the player controller's per-tick slots (vt+0x18 step, vt+0x38 quick-save/player
     list), or inside the system-controller command handler. Both are game-loop code. Menu opens and saves are only
     queued there (`pi+0x200` application-state list).
   - The mod should run the actions from a detour of `0x140454370` (SM 0x1404544d0), which the controller step calls at
     both of its exits with `controller+0x1c8`. It must never run them from the XR or present thread.
   - The highlight edits needed for the spots (§3.4) belong in the hud_vr wheel element detour. That is the thread that
     already runs the wheel's own selection code (R16 §2.2).
4. **Multiplayer.**
   - Quick save and quick load are offered only when `*(int*)(opts+0x08)` is non-zero. `opts =
     *(*(pi+0x10)+0x238)+0xd8`, the same struct as `0x14024c6a0(player)` and `0x14004e410(pi)`. The struct is
     unreflected; its +0x08 acts as "saving allowed", which is false in MP (the pad Back button toggles the player list
     there instead).
   - Quick save and quick load also need the player alive (`player+0x7a6` bit 0, both builds).
   - Netricsa needs `opts+0x5c`. Menu has no game gate besides `0x140aa7a80() == 0` (meaning unknown, §6).
   - Quick load through `vrwheel+0x58` has **no** gate of its own, so the mod must apply the save gate itself.
5. **Drawing.**
   - The spots can be drawn in the wheel's own HUD canvas, inside the hud_vr wheel branch, after `realElement` and
     before `flush`.
   - Helpers: the HUD text call `0x1400e8290`, the 9-slice box `0x1400e4580` and the texture quad `0x1400e8d20`, with
     the wheel's font (`wheel vt+0xa8`).
   - Background: the wheel params' gadget-list item texture.
   - Labels: the SS4 strings `ETRSVRSelection*Button.Tooltip*`, which still ship in both exes (§3).

Confidence: H for the Fusion layout and actions, the SS4 button natives and their SM ports, and the controller/syscmd
paths (decompile plus port in both builds). M for the call-with-stub approach and for the drawing plan: every helper is
used this way by the wheel itself, but the combination has not been run. Unknown items are listed in §6.

## 1. Fusion 2017 VR weapon selection

### 1.1 Classes (RTTI in the Fusion ELF)
| class | vtable | ctor | notes |
|---|---|---|---|
| CVRWeaponSelectionElement (base) | 0x2e59530 | — | 16 slots |
| CVRWeaponSelectionMiscButton | 0x2e595d0 | 0x1aea0d0 | shared button base: +0x20 render, +0x30 tooltip, +0x38 visible (= +0x40 ∧ player vt+0xec0 == 0), +0x48 ray test, +0x70 stretch |
| …MenuButton | 0x2e59670 | 0x1af1e40 | +0x40 avail 0x1ae6c00, **+0x50 act 0x1ae7e70**, +0x78 model `wwswi_pmcMenuButton`, +0x80 placement 0x1aef100 |
| …NetricsaButton | 0x2e59710 | 0x1af2780 | avail 0x1ae5d20, **act 0x1ae7730**, placement 0x1aef230 |
| …QuickSaveButton (`+0x40` = 1 save / 0 load) | 0x2e59a30 | 0x1af2b40 | avail 0x1ae5eb0, **act 0x1ae7a70**, model +0x120 save / +0x128 load, placement 0x1aef870 |
| …VoteButton / ReadinessButton / SwitchCameraButton / PlayerListButton / CheatsButton / VBSMenuButton / VBSCreateButton | 0x2e59990 / 0x2e59ad0 / 0x2e59b70 / 0x2e59c10 / 0x2e598f0 / 0x2e597b0 / 0x2e59850 | 0x1af2d40 / 0x1af23b0 / 0x1af1c80 / 0x1af25a0 / 0x1af2010 / 0x1af2960 / 0x1af21e0 | MP / dev buttons |
| …WeaponSlot / PowerUpSlot | 0x2e59cb0 / 0x2e59d50 | 0x1aefdf0 / 0x1af41e0 | |
| CVRWeaponSelectionWheelInfo (resource) | 0x2e560d0 | — | `vwswi_*` / `wwswi_*` members |

Wheel runtime functions:
- button builder 0x1af2f50 (order: Menu, Netricsa, VBSMenu, VBSCreate, Cheats, VoteYes/No, QuickSave, QuickLoad,
  Ready/Unready, Camera Next/Prev, PlayerList);
- open(hand) 0x1af3dd0 → place 0x1af0a10 → slots; fires the script event "WeaponSelectionStarted";
- per-tick 0x1af3a60: hover per hand through 0x1aebac0, plus slow-motion;
- close(hand) 0x1aea5d0: hovered element `vt+0x50(hand)`;
- info getter 0x1aec5f0.

### 1.2 Layout
Button placement (ctor, and again in slot +0x80) uses the **last (outer) ring**: `R = swr_fR`, `Dz = swr_fDz`, angle 0.
The rotation is identity (`DAT_03f7f230/238` = 0,0,0,1, set in `_INIT_1215`), the position is
(x, y, z) = (pos.x, pos.y, Dz), and +X is to the right of the wheel, +Y up:

| button | x | y |
|---|---|---|
| Menu | −(R+0.2) | 0 |
| Netricsa | −(R+0.2) | +0.15 |
| Player list | −(R+0.2) | +0.30 |
| VBS create / VBS menu | −(R+0.4) | 0 / +0.15 |
| **Quickload** | **+(R+0.2)** | **0** |
| **Quicksave** | **+(R+0.2)** | **+0.15** |
| Cheats | +(R+0.4) | +0.075 |
| Vote yes / no | −0.1 / +0.1 | R+0.2 |
| Ready | (top) | R+0.3 |
| Camera prev / next | −0.1 / +0.1 | (top) |

Content values (`WeaponWheelSelectionInfo.rsc`):
- rings: inner R 0.15, Dz −0.4, 5 slots; outer **R 0.30, Dz −0.30, 9 slots**;
- `vwswi_fDistanceFromPlayerController` 0.1;
- `vwswi_fStretch` 1;
- `wwswi_fWeaponSlotModelStretch` 0.1.

So in metres:
- Menu (−0.5, 0), Netricsa (−0.5, 0.15);
- Quickload (+0.5, 0), Quicksave (+0.5, 0.15);
- all in the wheel plane, 0.3 m along the wheel's −Z, scaled by the stretch `vrwheel+0xac` (`vr_fWeaponSelectionStretchOverride`
  0x3969344, default −1, or else `vwswi_fStretch`).

Models:
- `Content/SeriousSamFusion/Models/Interface/MenuWheel.mdl`, `NetricsaWheel.mdl`, `Icon_QuickSave_Wheel.mdl`,
  `Icon_QuickLoad_Wheel.mdl`;
- button backing `ClickableButtonOnWheel.mdl`;
- tooltips `ETRSVRSelection*` (§3.3).

Wheel placement 0x1af0a10, by `vr_vwsslWeaponSelectionLocation` (0x3f7e620, .bss, so default 0 = VWSSL_CONTROLLER):
- **0 controller:** the hand's controller pose (`0x1ac4b00(vrd, hand)`), roll removed, origin moved
  `vwswi_fDistanceFromPlayerController` (0.1 m) along the controller's forward.
- **1 head:** head yaw only, offset by `vr_fWeaponSelectionHeightDistanceFromHead` (0.4, .data 0x3969268) down and
  `…DepthDistanceFromHead` (0.65, 0x396926c) forward.
- **2 front:** head yaw snapped to 90° sectors.

In every mode the result is stored player-relative (`vrwheel+0x38..+0x50`). **The wheel is not offset to the side of the
hand. The side placement applies to the action buttons only.**

### 1.3 Selection and actions
- **Hover (0x1aebac0, per hand, each tick):**
  - The ray is the controller position plus the controller orientation's −Z.
  - For each visible element (`vt+0x38`), `vt+0x48` (0x1ae8e60) tests the ray against a sphere at the element of
    radius ½ × the larger model bounding-box extent.
  - A hit selects the element, and the hand's laser (vrd+0x3c8 / +0x4e0) is shown at the hit length.
  - Without a hit, the nearest element by closest approach is taken, but only if `vt+0x68` returns 0. Buttons return
    non-zero (stub 0x1ae5370), so **buttons need a direct hit**.
  - A change of hovered element plays `vwswi_presSlotHoverSound`.
- **Release** (close 0x1aea5d0): the hovered element's `+0x50(hand)` runs.
- **Availability `+0x40`:**
  - Menu: player present and `0x00eb9d00() == 0`.
  - Netricsa: the same, plus `opts+0x5c`.
  - QuickSave/QuickLoad: the same, plus `opts+0x08` (`0x1796170`) and player alive (`+0x7ae` bit 0).
- **Actions** (each first selects the hand's picked weapons, `0x1a6d510` right / `0x1a6d7b0` left):
  - Menu: `0x00ea61a0(0.5f, gameInfo, hand, "", 0, 0)` → `*(gi+0x370)+0xd4 = hand; 0x0f6fe10(gi, "", 1)`
    (default menu).
  - Netricsa: `player+0x1b54 (en_bHintNetricsaOrRodSelection) = 0; player+0x1fc0 = now`.
  - Quicksave: `player+0xe50 |= 1`, name `"Auto Save - <level>"` (0x0f45b10), `0x0f6bac0(gi, ctrlId, 0, name, 1,
    {0,1,1,1,1,1,0})`.
  - Quickload: `vrwheel+0x68 = 1`. The load runs later from the wheel tick.

## 2. SS4 / SM natives

### 2.1 The VR wheel's buttons (still in both builds)
| item | Sam4 | SamSM | evidence |
|---|---|---|---|
| button builder (= Fusion 0x1af2f50) | 0x140452f90 | 0x1404530f0 | tooltip strings, port_list |
| QuickSave(load) button ctor (`+0x40` = flag) | 0x14044faf0 | (not ported) | string ref |
| vtables Menu / Netricsa / QuickSave / base | 0x14184aca8 / 0x14184ad28 / 0x14184afa8 / 0x14184ac30 | 0x14182e688 / 0x14182e708 / 0x14182e988 / 0x14182e610 | `lea` in builder; slots match |
| **Menu act** `(btn, hand)` (vt+0x48) | **0x140451450** | **0x140451580** | sig_at 44/44 |
| **Netricsa act** | **0x1404514e0** | **0x140451610** | 43/44 (the only difference is field +0x1e28→+0x1e30) |
| **QuickSave/Load act** | **0x1404517c0** | **0x1404518f0** | 40/40 |
| avail Menu / Netricsa / QS (vt+0x38) | 0x140453f90 / 0x140453fe0 / 0x1404540c0 | 0x1404540f0 / 0x140454140 / 0x140454220 | vt slots, port |
| commit hand's VR pick `(vrwheel, hand)` | 0x140457390 | 0x140457530 | called first by every act; no-op when the hand list (`vrwheel+0x18`, count +0x20) is empty |
| VR wheel tick `(vrwheel)`: if `+0x58` → deactivate both hands, quick load | **0x140454370** | **0x1404544d0** | called with `controller+0x1c8` at 0x14040f8cc/0x14040f8ea (SM 0x14040f77c/0x14040f79a) |

What the acts do (Sam4 decompile; SM disassembly identical apart from field shifts):
- **Menu 0x140451450:** commit, then `0x140056c80(pi, hand, "", 0.5f, 0, 0)` (SM 0x140056ce0), which sets
  `*(pi+0x360)+0xd4 = hand` and calls `0x14094d9c0(pi, "", 1, 0.5f, 0, 0)` (SM 0x14094e2f0). This opens the default
  in-game menu; the request is queued on `pi+0x200`.
- **Netricsa 0x1404514e0:** commit, then:
  - `player+0x1e28` (`en_bHintNetricsaOrRodSelection`; SM +0x1e30) = 0;
  - `player+0x2340` = `now` (`0x1410c75f0(&t)`, SM 0x1410c7da0);
  - `player+0x2348` = invalid time `[0x1422566e8]` (SM `+0x2348` / `+0x2350`, `[0x142235048]`).
- **QuickSave 0x1404517c0** (`btn+0x40 != 0`):
  - commit;
  - `0x1404792d0(player, &key)` (SM 0x140479930);
  - `player+0xfa8` (`en_ulPlayerPuppetFlags`; SM +0xfb0) |= 1;
  - name = `0x1401cba00(&s, pi)` ("Auto Save - <level>"; SM 0x1401cb520);
  - `0x14094bc90(pi, &key, 0, s, 1, {0,1,1,1,1,1,0})` (SM 0x14094c1b0).
- **QuickLoad**, same function with `btn+0x40 == 0`: `*(int*)(vrwheel+0x58) = 1`. On the same tick's end, 0x140454370
  calls `0x1401ccd90(*(pi+0xb8), &key, 0)`.
- **Avail:**
  - Menu: player and `player+0x30` exist, and `0x140aa7a80() == 0`.
  - Netricsa: as Menu, plus `0x14024c6a0(player)+0x5c`.
  - QS: as Menu, plus `+0x08` of the same struct, plus alive.

**Calling them from the mod.** Build a zeroed 0x48-byte stub with `+0x08 = controller+0x1c8` (the vrwheel) and, for
save/load, `+0x40 = 1/0`. Then:
1. call avail;
2. if it returns non-zero, call act with `hand` 0 (right) / 1 (left).

The acts never read the stub's vtable. `vrwheel+0x00` must hold the player handle; the act's first line resolves it,
and does nothing if it is missing. Check this in the lab (§6.1).

### 2.2 Netricsa, desktop path
| item | Sam4 | SamSM |
|---|---|---|
| `plcmdDisplayNetricsa` handle (controller copy) | 0x1422859a0 | 0x142262af0 |
| reader: controller step vt+0x18 | 0x14040da90 (read at 0x14040f6eb) | 0x14040d940 (0x14040f59b) |

Gates: alive; `player vt+0xa28() == 0`; `0x14041dcf0(player+0x1070)+0x34 == 0`; wheel closed (`player+0x269c` bit 0,
**SM +0x26a4**); `0x14004e410(pi)+0x5c`.

The action toggles:
- if `+0x2340 == invalid` or `+0x2348 != invalid`, it opens: `+0x2340 = now`, `+0x2348 = invalid`, `+0x1e28 = 0`, slow
  motion `0x1401b9150(player, 3)` (SM 0x1401b8c90);
- otherwise it asks to close: `+0x2348 = now`.

Script `CPlayerActorPuppetEntity::InvokeNetricsa` 0x14045dd50 (SM 0x14045f860) does the same writes. When
`0x140ba6dd0(player)` is true it also bumps the counter at +0x2350/+0x2354 (SM +0x2358/+0x235c). It takes a script frame
(`vt+0x100`), so it is not callable directly. `InvokeNetricsaOnTab(_net)` 0x140423c40/0x140423cb0 call
`0x140429a90(player, tab)` (sets +0x2a34).

Pad binding (GBL_STANDARD): **D-pad Left** (`ButtonLeft`, priority 4), but the user's saved layout decides.

Note for R16 §5: the wheel-open bit is `player+0x26a4` in SM (controller code at 0x14040f634), not +0x269c. The mod's
`crosshair.cpp` already applies the delta.

### 2.3 Menu
| item | Sam4 | SamSM |
|---|---|---|
| system command handler (system controller vt+0x20) | 0x140156880 (ptr 0x1416145a8) | 0x140157520 (ptr 0x1415f7098) |
| `syscmdInvokeMenu` / `sysInvokeMenuOrExitTest` / `syscmdPause` | 0x142275b68 / 0x142275b70 / 0x142275bb0 | 0x142252c58 / — / 0x142252ca0 |
| open by name `(pi, name, fade, params, int* owner)` → `0x14094d9c0(pi,name,1,fade,params,owner)` | 0x140056c50 | 0x140056cb0 |
| menu request `(pi, name, queued, fade, params, int* owner)` | 0x14094d9c0 | 0x14094e2f0 |

`syscmdInvokeMenu` pressed and allowed (`DAT_142273a30 == 0`, world exists, not `0x140050700`, not "VRArcade") runs:
1. `0x140c9fcd0()`;
2. `0x140aa6ab0(pi+0xb8)`;
3. owner = `0x14004f7b0(pi, &out, &deviceOfBinding)`;
4. `0x140056c50(pi, "", 0.5f, 0, &out)`.

In VR the device's menu button (`0x140b6ade0`, device vt+0xe0(7)) is OR-ed in. The mod's pad Start already reaches
this. If no save exists, `0x1401ccea0` opens `"CMSInGameMenu2"` explicitly with `0x14094d9c0(pi, "CMSInGameMenu2", 1,
0.5f, 0, 0)`.

### 2.4 Quick save
| item | Sam4 | SamSM |
|---|---|---|
| `syscmdQuickSave` / `syscmdQuickLoad` (desktop keys) | 0x142275cc8 / 0x142275cd0 | 0x142252db8 / 0x142252dc0 |
| `plcmdQuickSaveTogglePlayerList` (pad Back) | 0x1422859d0 | 0x142262b20 |
| controller vt+0x38 (pad quick save / player list) | 0x1404168e0 | 0x1404167f0 (sig 56/56) |
| save request `(pi, int* ctrlId, 0, char* name, 1, int params[7])` | 0x14094bc90 | 0x14094c1b0 |
| quicksave name `(CString* out, pi)` (level name) | 0x1401cbae0 | 0x1401cb600 |
| "save in progress" box `(menuMgr)` → 1 if busy | 0x140aa6ab0 | 0x140aa71f0 |
| controller entry `(pi, int* key)` (entry +8 = id) | 0x14004f850 | 0x14004f8b0 |
| key of a player `(player, int* out)` | 0x1404792d0 | 0x140479930 |
| game-option struct of pi / of entity (`*(*(pi+0x10)+0x238)+0xd8`) | 0x14004e410 / 0x14024c6a0 | 0x14004e470 / 0x14024c2c0 |
| save gate getter `opts+0x08` / alive `+0x7a6&1` | 0x14008d380 / 0x14029daf0 | 0x14008d580 / 0x14029d8c0 |
| params[0] global (unnamed) | `[0x142247068]` | `[0x142228018]` |

Desktop (pad and key) sequence, in controller vt+0x38 and in the syscmd handler alike:
```
if (*(int*)(opts+8) && player && alive && pressed) {
  player+0xfa8 |= 1;                          // SM +0xfb0
  0x140aa6ab0(*(pi+0xb8));
  CString s; 0x1401cbae0(&s, pi);
  int* id = (int*)(0x14004f850(pi, &controller+0xc) + 8);
  int p[7] = { *(int*)0x142247068 == 0, 1,1,1,1,1, 0 };
  0x14094bc90(pi, id, 0, s.ptr, 1, p);  dtor(s);
}
```
The syscmd copy also requires `0x1400512e0(pi) == 0` (pi+0xd0 vt+0x80) and `*(*(pi+0x10)+0x208) == 0`; their meanings
are unknown. In MP (`opts+8 == 0`) pad Back opens `CMSGameOverlay` (player list) instead.

The menu's `QuickSave_OnClick` 0x14087c5c0 (SM 0x14087c540) and the VR button pass `p[0] = 0`. The VR button also names
the save "Auto Save - …".

### 2.5 Quick load
| item | Sam4 | SamSM |
|---|---|---|
| syscmd branch: alive → `0x1404187c0(player)` (if `player+0x2160 == 0`) | 0x1404187c0 | 0x1404186d0 |
| quick load `(menuMgr = pi+0xb8, int* key, int)` | **0x1401ccd90** | **0x1401cc8b0** |
| load latest or open CMSInGameMenu2 `(pi)` | 0x1401ccea0 | 0x1401cc9c0 |
| VR wheel deferred flag | `controller+0x1c8+0x58` | same |

`0x1401ccd90`:
- if `0x140b7cc30(pi)` is set and `0x140049510()` returns 0, it restarts the level (`0x1400556c0`, meaning unclear);
- otherwise it gets the controller entry, and if entry `+0x88` is set (probably `prj_bMustConfirmQuickLoad`
  0x142247cc4 / SM 0x14222899c) it shows the "Are you sure…" dialog; otherwise `0x1401ccea0`.

`CMSInGameMenu2::QuickLoad_OnClick` 0x14087c570 (SM 0x14087c4f0) ends in the same `0x1401ccd90`. Dead-player and
Talos-rewind (`player+0x2160`) branches exist in 0x14040d460 / 0x1404a0480; the wheel does not need them.
`prj_ctMaxQuickSaves` (0x1422472e0 / SM 0x1422281e0) caps the slot rotation.

## 3. Drawing the spots on the wheel panel

### 3.1 Where
In `hud_vr.cpp hookedElement`, wheel branch (`onHand`):
1. after `realElement(ctx, e)`, and before `flush(ctx)` and the restore of `native` placement/anchor, draw the spots;
2. they then use the same panel matrix as the wheel.

Coordinates are drawport pixels (the wheel's own space). `hud = ctx[10]`, batch = `*(hud+0xb0)`, gfx = `[0x1422aa9b0]`
(SM `[0x142287b70]`), fade = `*(float*)(wheel+0xc0)`.

### 3.2 Helpers (as called by wheel 0x1401298c0 and gadget slot 0x140128550)
| helper | Sam4 | SamSM | call |
|---|---|---|---|
| text | 0x1400e8290 | 0x1400e8360 | `(batch, gfx, CString* txt, TextParams* tp, uint64 xy /*x low float, y high float*/, float alpha)` |
| 9-slice box | 0x1400e4580 | 0x1400e4650 | `(batch, gfx, CTexture* tex, float rect[4] /*x0,y0,x1,y1*/, uint32 ARGB)` |
| texture quad | 0x1400e8d20 | 0x1400e8df0 | same args; `tex vt+0x80/+0x88` = width/height |
| TextParams init / size / free | 0x140b53a70 / 0x140b550c0 / 0x140b53e30 | 0x140b53f70 / 0x140b555c0 / 0x140b54330 | `init(tp[0x60], font+0x30)`; `size(tp, 1, s)` writes tp+0x20; tp+0x10 colour, tp+0x14 flags (`|= 0x200`, centred, as wheel), tp+0x38 = −1.0f |
| wheel font `wheel vt+0xa8` | 0x1400da470 | 0x1400da550 | returns font params; use `+0x30` |
| translate `"ETRS..=Default"` → text | 0x1410f8e10 | 0x1410f9720 | then CString ctor 0x1410bfac0 / dtor 0x1410bfce0 (SM 0x1410c0270 / 0x1410c0490) |
| HUD unit `0x1400da500(wheel)` | 0x1400da500 | 0x1400da5e0 | px unit = `hud(wheel)/hud_fCustomHudScale × hud_fWeaponWheelScale` (0x142247550 / 0x142247604; SM 0x142228440 / 0x1422284f4) |
| wheel centre `wheel vt+0x98(wheel, &out, &{320,240})` | 0x1400db320 | 0x1400db400 | gadget list origin = 0x1401261e0 (SM 0x140126b90) |
| text flush (end of HUD) | 0x1400ea890 | 0x1400ea940 | already called by HUD render |

Text size as the gadget title: `params+0x17c × params+0x19c / hud_fCustomHudScale × hud_fWeaponWheelScale`. Alpha in a
colour = `(A × fade) << 24 | RGB`.

### 3.3 Assets
Wheel params (`CWeaponSelectionWheelHudElementParams` = `*(wheel+0x18)`, the same offsets in SM):
- `hep_ptexGadgetListItemTexture` +0x1c8 as spot box;
- `hep_colGadgetListItemBackgroundColorInactive` +0x1d0 / `ForegroundColor` +0x1d4 for idle;
- `hep_colSelectedElementBorderColor` +0x68 / `HoveredElementBorderColor` +0x58 for hovered.

Native strings (VAs):
| string | Sam4 | SamSM |
|---|---|---|
| `ETRSVRSelectionNetricsaButton.Tooltip=Open Netricsa` | 0x14184b378 | 0x14182ed58 |
| `ETRSVRSelectionMenuButton.Tooltip=Open the Menu` | 0x14184b348 | 0x14182ed28 |
| `ETRSVRSelectionQuickSaveButton.TooltipSave=Quicksave` | 0x14184b4b0 | 0x14182ee90 |
| `ETRSVRSelectionQuickSaveButton.TooltipLoad=Quickload` | 0x14184b4e8 | 0x14182eec8 |
| `ETRSPlayerCommand.QuickSave=Quick Save` / `.QuickLoad=Quick Load` | 0x1416153f0 / 0x141615418 | 0x1415f7ee0 / 0x1415f7f08 |
| `ETRSMenu.Netricsa=Netricsa` / `ETRSMenu.Menu=Menu` | 0x141ba69d0 / 0x141b9c780 | 0x141b8b220 / 0x141b80fd0 |

Translations of the `ETRSVRSelection*` keys may be missing in SS4's language files, in which case the default after "="
is shown. `ETRSPlayerCommand.*` / `ETRSMenu.*` are used by the desktop UI.

Icons: `mp_ptxQuickSave` (menu params +0x238, the "Quicksave icon") and `mp_ptxNetricsaHudPanel` (+0x4b8) exist. The
menu-params owner pointer was not traced. The Fusion `*Wheel.mdl` icons are 3D models, not HUD textures. Text-only spots
on the gadget-list box are the safe choice.

### 3.4 Selection without a native wheel change
- Close (0x14012be40) commits `wheel+0x78` (primary hand highlight) or, if null, `+0x68` (current weapon); likewise
  `+0x80` / `+0x70` for the second hand (R3).
- While the hand points at a spot:
  - feed the wheel stick as 0, so the native hover stops updating;
  - in the wheel element detour, before `realElement`, set `wheel+0x78 = 0` (and `+0x80 = 0`, `+0xbc = 0`).
- Release then re-commits the current weapon (the same as releasing the desktop wheel without moving), and the mod
  queues the spot's action.

## 4. Ports (summary)
Function pairs Sam4 → SM:
- controller step vt+0x18 0x14040da90 → 0x14040d940 (vtables 0x14182e920 / 0x141812308; vt+0x38 0x1404168e0 →
  0x1404167f0, vt+0x48 0x14040d460 → 0x14040d2c0);
- VR tick 0x140454370 → 0x1404544d0;
- button acts / avails / commit as in §2.1;
- save request 0x14094bc90 → 0x14094c1b0;
- quick load 0x1401ccd90 → 0x1401cc8b0;
- menu request 0x14094d9c0 → 0x14094e2f0;
- open-with-hand 0x140056c80 → 0x140056ce0;
- entity→pi 0x14024c710 → 0x14024c330;
- entity→opts 0x14024c6a0 → 0x14024c2c0;
- draw helpers as in §3.2.

Player field shifts in SM: +0x1e28 → +0x1e30, +0x2340/+0x2348 → +0x2348/+0x2350, +0xfa8 → +0xfb0, +0xf70 → +0xf78,
+0x269c → +0x26a4. Unchanged: +0x7a6, controller+0x1c8, vrwheel+0x58, wheel params offsets.

The Unrestricted builds were not ported.

## 5. Fusion → SS4 correspondence
| Fusion | SS4 |
|---|---|
| vrwheel (controller VR wheel) | `controller+0x1c8` (R3) |
| button builder 0x1af2f50 | 0x140452f90 |
| Menu/Netricsa/QS act 0x1ae7e70 / 0x1ae7730 / 0x1ae7a70 | 0x140451450 / 0x1404514e0 / 0x1404517c0 |
| quickload flag `vrwheel+0x68` | `vrwheel+0x58` |
| netricsa fields `+0x1b54` / `+0x1fc0` | `+0x1e28` / `+0x2340` (+0x2348 = invalid) |
| quicksave `0x0f6bac0` | `0x14094bc90` |
| menu `0x00ea61a0` | `0x140056c80` |

## 6. Unknowns and runtime checks
1. Whether `controller+0x1c8` is initialised (player handle at +0x00) when the VR wheel was never activated. If the
   handle is not there, the act functions do nothing, and the desktop sequences of §2.2–2.5 are used instead. These
   sequences need the same natives, apart from the stub.
2. Meanings of the remaining gates:
   - `0x140aa7a80()` (`0x140049120(...) == [0x142295568]`; Fusion `0x0eb9d00`);
   - `0x1400512e0(pi)`;
   - `*(*(pi+0x10)+0x208)`;
   - the restart branch inside `0x1401ccd90`;
   - params[0] `[0x142247068]`.
3. Whether controller entry `+0x88` is `prj_bMustConfirmQuickLoad`.
4. The values of `hep_vGadgetsListOffset` and of the ring radii in SS4 content, which decide where the spots fit in the
   drawport (the right-hand side may hold the gadget list).
5. Which thread calls controller vt+0x18 / system controller vt+0x20. Static analysis shows only that they are dispatched
   through vtables from game-loop code. The plan relies only on "same function as the native".
6. Pad D-pad Left / Back bindings depend on the saved gamepad layout; avoid them unless they are verified.
7. `*(pi+0x360)+0xd4 = hand` in the menu-with-hand path is probably the VR menu pointer hand (not traced).

## Recommended implementation

1. **Symbols** (manual.json, Sam4 + SamSM):
   - `vrwheel_tick`, `vrbtn_menu_act`, `vrbtn_netricsa_act`, `vrbtn_quicksave_act`, `vrbtn_menu_avail`,
     `vrbtn_netricsa_avail`, `vrbtn_quicksave_avail`;
   - `hud_text`, `hud_box9`, `textp_init`, `textp_size`, `textp_free`, `translate`, `cstr_ctor`, `cstr_dtor`;
   - the `ETRSVRSelection*` string VAs.

   Port the Unrestricted builds before use (fingerprint gate).
2. **Spots (wheel_panel.cpp + hud_vr.cpp):**
   - Fusion layout, in wheel pixels: left column Menu at (−(Rpx+m), 0) and Netricsa at (−(Rpx+m), −0.5·Rpx); right
     column Quick load at (+(Rpx+m), 0) and Quick save at (+(Rpx+m), −0.5·Rpx). Fusion's 0.2 m / 0.15 m at R = 0.3 m
     gives m ≈ 0.67·R and a 0.5·R step; y is negative upward in pixels.
   - If the gadget list sits on one side, put all four in a column on the other side.
   - Hide a spot whose avail function returns 0 (no Quick save / Quick load in MP or when dead; no Netricsa when the game
     disables it).
   - Draw each spot with `hud_box9(params+0x1c8)` plus `hud_text(label)`, coloured hovered or idle and scaled by fade,
     inside the wheel branch after `realElement`.
3. **Hover:**
   - Project the opening hand's displacement (the same vector the mod already feeds as the stick) onto the panel. Use
     the panel's metres-per-pixel `height / h` from hud_vr, so pixels = displacement / (height/h), with y flipped.
   - When that point lies inside a spot rectangle and beyond the outer ring, the spot is hovered: feed stick 0 and clear
     `wheel+0x78/+0x80/+0xbc` before `realElement` (§3.4).
   - Pulse haptics once when the hovered spot changes (Fusion plays the hover sound).
4. **Action on release:**
   - When `wheelOpen()` goes false with a spot hovered, latch `{action, hand}`.
   - In a detour of `vrwheel_tick` (Sam4 0x140454370 / SM 0x1404544d0), for the local VR player's controller only (arg
     = controller+0x1c8):
     1. build the 0x48-byte stub `{+8 = arg, +0x40 = save?1:0}`;
     2. call avail, then act(hand) (engine hand 0 = right, 1 = left);
     3. call the original tick, which runs a quick load in the same tick.
   - Fallback if check 6.1 fails:
     - Netricsa: the writes of §2.2 (with slow-mo 3), gated as there;
     - Menu: `0x14094d9c0(pi, "CMSInGameMenu2", 1, 0.5f, 0, 0)`, or a pad Start pulse;
     - Quick save: the §2.4 desktop sequence;
     - Quick load: set `*(int*)(arg+0x58) = 1` before calling the original. Gate it on `opts+8` and alive yourself;
       this path has no gate.
5. **Nothing new is replicated.** Saving and loading are host-only and gated off in MP. Netricsa and the menu are local,
   the same as the desktop keys.
6. **Lab checks**, in order, for SS4 and then SM:
   1. The log shows the avail results with the wheel open.
   2. Each spot is drawn on the hand panel and follows the fade.
   3. Releasing on a spot leaves the weapon unchanged.
   4. Menu opens the in-game menu.
   5. Netricsa opens and closes with the native slow motion.
   6. Quick save writes a save; check whether it is labelled "Auto Save - …" (button path) or quick-save named (desktop
      path).
   7. Quick load asks for confirmation when `prj_bMustConfirmQuickLoad` is set.
   8. In MP, Quick save and Quick load are hidden.

## 7. Drawing text from the mod (spot labels)

Lab symptom (SamSM 628492, also Sam4 616154): inside the thunk on the wheel render's own `textparams_free` call
(`hud_vr.cpp wheelTextDone`, SM site 0x14012a7db), `hud_box9` with `params+0x1c8` shows on the panel, but `hud_text`
with a magenta colour draws nothing. Static only. Decompiles were made from a scratch copy of `lab/ghidra_r19`; SM was
checked by disassembly.

### 7.1 Answer
**The text size is in the wrong unit.** It is about 100× too large. Every other argument matches the native callers.
- `textparams_size(tp, 1, s)` stores `s` at `tp+0x20`. That value is a **multiplier of the font's cell height**
  (`*(int*)(font+0x68)`, font = `tp+8`), not a pixel size.
- The text path multiplies it again by the HUD unit when it queues the text (§7.2).
- Native wheel and gadget-slot labels pass `stretch × hep_f…TextSize / hud_fCustomHudScale × hud_fWeaponWheelScale`.
  In shipped HudParams that is 1.151 × 0.27 ≈ **0.31** for gadget names and 1.151 × 0.399 ≈ **0.46** for the
  "GADGETS" title.
- `drawSpots` passes `13 × unit` (30–70 in the lab), and `unit` already contains the HUD unit.
- Each glyph cell is therefore 30–70 font heights × the HUD unit, i.e. thousands of drawport pixels. The pen starts at
  the spot, and even the cell's top padding/bearing (scaled the same way) is larger than the drawport. Only transparent
  texels, or nothing, land in the image, so the magenta search finds nothing.
- The box works because `hud_box9` takes a pixel rectangle.

### 7.2 What the text path does (Sam4 / SM, read from code)
| step | Sam4 | SamSM | notes |
|---|---|---|---|
| `hud_text` | 0x1400e8290 | 0x1400e8360 | Only checks `canvas != 0`. Calls `0x1400d9990(*canvas, gfx)` (SM 0x1400d9a70) = HUD unit, the same function as `0x1400da500(wheel)`. Then calls the body with it as a 7th argument. |
| body (left/top aligned) | 0x1400e8340 | 0x1400e8410 | Finds or creates the canvas entry for (layer `canvas+0x10`, drawport `gfx+0x1c78`); the matrix is captured only at creation. Appends a 0xa8 text record: `+0` xy (copied verbatim), `+8` CString, `+0x10` TextParams copy (0x140b53e70), `tp.size(+0x20) *= HUD unit`, `+0x70` colour = `(A(tp+0x10) × alpha) << 24 \| RGB(tp+0x10)`. **Always queued**: `hud_bAllowBatching` is not read. |
| centred variant | 0x1400e87f0 | 0x1400e88c0 | Same 6 arguments `(canvas, gfx, CString*, tp, xy, alpha)`, used by the weapon/gadget slots (0x14012aa88, 0x140128cb6). Scales the size by the HUD unit, measures the string (0x140b938c0 / SM 0x140b93f30), stores `xy − size/2`. **xy is the text centre.** |
| flush | 0x1400ea890 | 0x1400ea940 | Per text record: `0x140b56d60(&rec.tp)` (current TextParams), then gfx text 0x140b953c0 `(gfx, rec.str, {x, y, 0}, rec.colour)` with the entry's stored matrix (3D) or pixel ortho (`dp+0x30`). |
| gfx text early-outs | 0x140b953c0 | – | Needs a drawport (`gfx+0x1c78`), a non-empty string, a current TextParams and its font (`tp+8`). Nothing else: no markup, translation or text-batch start is needed. |

TextParams fields (init copies `font+0x30..+0x5f`; 0x140f46840 builds the glyph metrics):
- `+0x08` font. Must be non-null; the weapon slot tests it before drawing.
- `+0x10` colour. The top byte is alpha, so alpha 0 is invisible. The low 24 bits use the same order as the `hud_box9`
  colour, and magenta/grey are unaffected by an R/B swap.
- `+0x14` flags:
  - bit0 bold widening;
  - bit1 slant;
  - bit8 fixed advance;
  - **0x200 = caps**: lower-case is drawn as upper-case scaled by `+0x2c` (char class bit 8 / case table at 0x1410c4510 /
    0x1410c4550). It is **not** centring.
- `+0x18` spacing.
- `+0x1c` line spacing.
- `+0x20` size (× `font+0x68` px).
- `+0x24` horizontal stretch.
- `+0x30` / `+0x38` / `+0x3c,+0x40` shadow colour / shadow strength (`> 0` enables it; −1 = off, as the wheel sets it) /
  shadow offset.

`textparams_size` modes (0x140b550c0 / SM 0x140b555c0, identical):
- 1: `+0x20 = s`;
- 2: `s × drawport height / font+0x68`;
- 3: `s × (*dp)+0x24 / font+0x68`;
- 8: `s / font+0x68`, i.e. a cell height of `s` HUD pixels before the HUD unit.

Other points:
- xy is in the same canvas coordinates as the `hud_box9` rectangle; the wheel computes both from vt+0x98 / 0x1401261e0.
  For the body, x is the pen start and y is the **top** of the line (a newline adds the line height), not the baseline.
- `alpha` scales the colour's alpha byte. `wheel+0xc0` (fade) is correct.
- The CString is a plain `char*` (0x1410bfac0 → 0x1410c1800 copies the bytes), so `"Menu"` is fine. The translate call
  is only needed for `ETRS…=` keys.

All 12 `hud_text` callers in each build (0x1400c87e2 … 0x140129e0c; SM 0x1400c8992 … 0x14012a7ac) and the 8 centred
callers pass the same canvas `*(*(elem+8)+0xb0)`, the gfx global, a stack CString, the TextParams after init/flags/−1,
packed xy and `fade`. They differ from the mod **only in the size value**.

### 7.3 Fix (both builds, `hud_vr.cpp drawSpots`)
1. Size, as the gadget-name label (0x140128550):
   ```
   float s = *(float*)(params+0x17c) /*hep_fGadgetListStretch*/ * *(float*)(params+0x1a8) /*hep_fGadgetNameTextSize*/
             / hud_fCustomHudScale * hud_fWeaponWheelScale;   // Sam4 0x142247550 / 0x142247604, SM 0x142228440 / 0x1422284f4
   g_sd.tpSize(tp, 1, s);                                      // ≈ 0.31; do NOT multiply by `unit`
   ```
   Use `+0x19c` (title size, ≈0.46) for larger labels. Equivalent alternative: `tpSize(tp, 8, 13.0f)` for a 13 HUD-pixel
   cell, still without `unit`.
2. Centre the label with the centred variant, **Sam4 0x1400e87f0 / SM 0x1400e88c0** (same signature as `hud_text`):
   pass `xy = {p[0], p[1]}`, the spot centre, and drop the `- textSize*0.5`. Add it to manual.json as `hud_text_centered`
   for Sam4 and SamSM (the Unrestricted builds are not ported).
   - If you keep `hud_text`, x is the left edge and y the top.
   - Keep `|= 0x200` (all caps, as the wheel), the −1.0 at `+0x38`, the colour and `fade` unchanged.
3. The rest of the setup is already correct, so leave it as it is:
   - drawing inside the wheel render (same canvas entry and matrix as the wheel's own labels);
   - the per-element `flush(ctx)` after `realElement`;
   - `hud_bAllowBatching = 0`, which does not affect text.

Runtime check: once, log `*(int*)(*(uint8_t**)(tp+8)+0x68)` (cell px) and `*(float*)(tp+0x20)`. Expect roughly 0.3–0.5,
and a few tens of drawport pixels for cell × size × HUD unit.

### 7.4 Lab result: the 3D HUD path draws no text (2026-10-26, Sam4)
With the size fixed (§7.3), the labels still did not show, and neither did the wheel's own text (ammo counts,
"GADGETS", gadget names) on the hand panel. Findings:
- The queue is fine: the `gfx_text` diag hook sees every label at its canvas position, white, size 0.249 × 50 px cell
  × HUD unit 2.35 ≈ 29 px.
- The text flush (0x1400ea890) destroys every entry after drawing, so no stale matrix carries over between elements;
  its quad and text sections take the same 3D setup (device slots 0x070/0x078, keyed by `device+0x08`).
- Turning the panel 180° hides the wheel's quads (back faces culled) and still shows no text from either side, and
  no text lands at the raw pixel positions in the eye image. The 3D text path draws nothing visible; SS4 never
  shipped VR, so nothing exercised it.
- Text on the 2D path works (the flat HUD's "CHEATS" label).

Fix (`hud_vr.cpp panelText`): while the wheel renders on the hand, `hud_text` and `hud_text_centred` are hooked.
For each call:
1. Flush what the wheel queued so far, which is drawn on its 3D path.
2. Project the text point from the panel into this eye's pixels, using the element anchor, `eyeInHead` and `eyeFov`.
3. Scale `tp+0x20` by the panel pixel's size at that depth.
4. Queue the text with `dp+0x30 = 1` (2D) and flush.
5. Restore `dp+0x30`, the size and the object matrix at `gfx+0xcd0..+0xd30`, which the flush rewrites and the next
   queue entry captures.

Per-eye projection keeps the text at the panel's depth in stereo. Lab: spot labels on their boxes, and the wheel's
title and gadget names visible on the panel. Option `wheel_panel_text` (1).
