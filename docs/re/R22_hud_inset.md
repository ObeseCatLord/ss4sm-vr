# Gate R22: lay the flat VR HUD out inside an inset rectangle (Sam4.exe 616154, SamSM.exe 628492)

Static analysis only. Sources: capstone disassembly of all four exes, Ghidra decompiles from a private scratch copy of
`lab/ghidra_r16` (functions listed per section), the private content extract `lab/r8_content/.../HudParams.rsc`,
`tools/{callers,vtable_slot,class_vtables,meta_members,field_refs,cvar_readers,port_list,sig_at,sigscan}.py`. Nothing was
run. Addresses are Sam4.exe PE VAs unless marked. "layout" = the `CHudElementsParamsDatabase` at `hud+0xa0`; "unit" =
canvas pixels per virtual (640×480) pixel; "e" = a `CHudElement`, `e+0x08` = its `CHud`, `e+0x18` = its params.

## 0. Answers

1. **One function maps every screen-anchored position: `0x1400dd1a0` (layout map, SM `0x1400dd270`).** It turns a
   point in the virtual 640×480 layout into canvas pixels of the current drawport with three-zone anchoring (left /
   centre / right third, top / middle / bottom third), the **engine's own safe area** as the margin, and the unit as
   the scale. Every element reaches it through `e vt+0x98` (point), `e vt+0xa0` (rect) or the `CSimpleHudElement`
   override of `vt+0x98`; the objective text and the message family call it directly. Element **sizes** come from the
   unit functions next to it, which read the same safe-area getter.
2. **The engine already has an "inset" feature: the safe area.** `0x140b99060` returns `(sx, sy)`; in `0x1400dd1a0` the
   margins are `(1 − s)·W·pos` and the unit is `graph(H)·hud_fCustomHudScale·sy`. So a safe area of 0.65 lays the HUD
   out inside the central 65 % with sizes scaled by 0.65, which is exactly the requested behaviour.
3. **Why gfx_fSafeAreaX/Y did nothing useful:** the getter clamps them to `[0x14224cf50, 0x14224cf58]` = **[0.85, 1.0]**
   (static defaults) and **writes the clamped value back into the cvar**, and it ignores them entirely when
   `0x14224cf38 == 0` (then it returns `gfx_fSafeAreaSDX/SDY`). At best that is a 7.5 % margin, and it rewrites a
   persistent cvar. The mod must not use the cvars.
4. **World-projected elements do not use the layout map for their projected points.** Crosshair, aiming indicator,
   player probe, auto-aim, damage numbers, lock-on targets and critical hits project with `0x141152ae0` and the
   drawport size directly. Only their **sizes** go through the unit (safe-area `.y`). Objective markers project
   directly too, but the on-screen box that decides "draw at the projection" vs "clamp to the edge with an arrow" is
   built with `vt+0x98`, so it would inset (§3.3).
5. **Recommended hook (all four builds, same relative offsets): retarget the safe-area `E8` calls inside the layout
   functions to two mod thunks, and turn them on per element in the existing `hud_element_render` detour.** One call
   site (`0x1400dd273`) controls positions only, seven control sizes only, and one (`0x1400dd2e4`, the safe-area *pos*
   getter) controls where the inset rectangle sits, so it can be centred on each eye's optical axis. No cvar is written,
   no params are written and the drawport is not touched. §5 has the sites, bytes and thunk code.
6. A drawport-box or 2D-matrix override was rejected (§4). The drawport size is the viewport the projected elements use,
   queued quads are drawn at the end with a fresh pixel ortho per entry, and some elements replace the object matrix.

Confidence: high for the mapping, the getter, the call sites and their bytes (read in all four builds). High that the
projected elements listed do not call the layout map. Medium for the class names (nearest .rdata string heuristic,
§3) and for the subtitles element (not traced to the end).

## 1. The layout mapping

### 1.1 Chain
| step | Sam4 | SamSM | notes |
|---|---|---|---|
| element point map, base `vt+0x98` | 0x1400db320 → 0x1400d9d20(out, hud, in, gfx) | – (port by vtable) | `hud_map` 0x1400d9d20 = `dd1a0(*(hud+0xa0), out, in, gfx)` |
| element rect map, base `vt+0xa0` | 0x1400db2b0 → 0x1400d9c90 → **0x1400dceb0** | 0x1400db390 (sig short) → … → 0x1400dcf80 | corner via dd1a0, size × unit |
| `CSimpleHudElement` `vt+0x98` | **0x14010da40** | 0x14010e350 | `map(anchor e+0xd8) + unit·(p − anchor)`; anchor = `hep_vPosition`, set in render 0x14010c580 |
| **layout map** | **0x1400dd1a0** | **0x1400dd270** | 5 direct callers: 0x1400d9d20, 0x1400dceb0, 0x140110060/0x140110140 (one element), 0x140094030 (icon helper, also used by menu code 0x1408da690) |
| unit (layout, gfx): `custom·graph(H)·sy` | 0x1400dc930 (via 0x1400d9990 / `e` 0x1400da500) | 0x1400dca00 | `H` = drawport height |
| unit at a fixed height: `custom·graph(arg)·sy` | 0x1400dc8e0 | 0x1400dc9b0 (+0xd0 shift, like every layout function; its call site is verified) | used by 0x1400d99a0 = `H·dc8e0(1080)/1080` (crosshair, aiming indicator, object holder, discussion) |
| scale rect / vector (no offset) | 0x1400dcfb0, 0x1400dd110 (via 0x1400d9cc0 / 0x1400d9cf0, `e` 0x1400db300) | 0x1400dd080, 0x1400dd1e0 | sizes |
| safe-area getter `(sx, sy)` | **0x140b99060** | **0x140b99710** | §2 |
| safe-area position getter `(px, py)` | **0x140b990d0** | **0x140b99780** | `gfx_fSafeAreaPosX`, `gfx_fSafeAreaPosY/0x14224cf04`, clamped to [0, 1] |
| layout ctor | 0x1400dc4c0 | 0x1400dc5a0 | sets `+0x458/+0x45c` = 640/480, vtable 0x1415dfad0 |

Correction to R8 §1 / R16 §2.2: 0x1400d9990/0x1400da500 is `custom·graph(H)·sy`, **not** `H × factor / 1080`. The
`/1080` form is 0x1400d99a0 (`H·graph(1080)·custom·sy/1080`), which only the crosshair, aiming indicator, object
holder and discussion element use.

`graph` is `hed_pgrResolutionScaleGraph` (`CHudElementsParamsDatabase+0x60`, reflection; same offset in SM), called as
`vt+0x50(height)`. SS4 content: a linear graph through (0, 0.469) and (5000, 6.682), so
`graph(H) ≈ 0.469 + 0.0012426·H`. For the lab eye (1344×1512) the unit is ≈ **2.35 px per virtual px**, so the virtual
640 px are 1503 px, wider than the 1344 px image. The side zones already crowd the centre on this portrait drawport.

### 1.2 The map, read from 0x1400dd1a0 (byte-identical in all four builds)
```
dp = gfx+0x1c78; if (!dp || !*dp) return layout+0x458 (640,480)
W = dp.x1-dp.x0 (+0x18-+0x10), H = dp.y1-dp.y0 (+0x1c-+0x14), raster = *dp (+0x20 width, +0x24 height)
unit = graph(H) * hud_fCustomHudScale * SA1.y          // SA1 = safe_area() at +0x95 (0x1400dd235)
(sx, sy) = SA2                                         // SA2 = safe_area() at +0xd3 (0x1400dd273)
if (dp.x1 < raster.w/2) sx = 1 - (1-sx)*1.5            // split-screen halves only
if (dp.y1 < raster.h/2) sy = 1 - (1-sy)*1.5
(px, py) = safe_pos()                                  // at +0x144 (0x1400dd2e4)
px = (dp.x0 >= 1) ? 0 : (dp.x1 < raster.w && dp.x0 == 0) ? 1 : px   // likewise py; full-raster dp keeps pos
VW = 640, VH = 480, v = input point (virtual)
x = v.x <= VW/3   ? (1-sx)*W*px + v.x*unit                                   // left third: left anchored
  : v.x <= 2VW/3  ? (v.x - VW/2)*unit + W*(sx/2 + (1-sx)*px)                  // centre third: centred
  :                 W - (VW - v.x)*unit - (1-px)*(1-sx)*W                     // right third: right anchored
y = same with VH, H, sy, py
```
The result is relative to the drawport origin, in canvas pixels. Constants: 0x14156af0c = 1.0, 0x14156af18 = 3.0,
0x14156af00 = 0.5, 0x1415dfc18 = 1.5 (double).

The **inset rectangle** is `x ∈ [(1−sx)·W·px, (1−sx)·W·px + sx·W]`, and its centre is `W·(sx/2 + (1−sx)·px)`. The
centre-zone elements are centred on that point. With `px = py = 0.5` it is the drawport centre.

### 1.3 Example (Health, `hep_vPosition` (17, 440), lab eye 1344×1512, unit 2.35)
* Native (s = 1): x = 40, y = 1512 − 40·2.35 = 1418. That is the bottom-left corner of the eye image.
* Positions at s = 0.65, sizes at 0.65: margins 235/265 px, unit 1.53, so x = 261, y = 1186.

## 2. The safe-area getter 0x140b99060 (SM 0x140b99710)
```
if (*(int*)0x14224cf38 == 0) return (gfx_fSafeAreaSDX 0x14224cee8, gfx_fSafeAreaSDY 0x14224ceec)   // static 0.85, 0.85
x = max(min(gfx_fSafeAreaX 0x14224cef0, [0x14224cf58]), [0x14224cf50])  // static max 1.0, min 0.85
y = max(min(gfx_fSafeAreaY 0x14224cef4, [0x14224cf5c]), [0x14224cf54])
gfx_fSafeAreaX = x; gfx_fSafeAreaY = y                                   // writes the clamped value back
return (x, y)
```
* `0x14224cf38` (unnamed, static 1) is written in gfx init at 0x140b66f3d. The minimums 0x14224cf50/54 are rewritten at
  startup (0x14003c955, 0x14003d630; source value not traced).
* There are 34 direct callers (menus, layout, controller debug text, …). So the getter itself must not be detoured
  globally. The layout's own calls are retargeted instead.
* `htc_bIgnoreSafeArea` (`CHudTextConfig+0x14`) exists, but all 17 instances in SS4 `HudParams.rsc` are FALSE.

## 3. Which elements use what

Classification: every vtable holding the `CHudElement` base `vt+0x70` (0x1400db1c0) or `vt+0x98` (0x1400db320), then
the transitive direct calls of `vt+0x80` (render) to depth 4, plus virtual calls `[reg+0x98]`. Class names are the
nearest preceding .rdata class string (heuristic; the ones checked against content and decompiles are marked ✓).

### 3.1 Screen-anchored: positions through the layout map (inset by §5)
| class (render vt+0x80) | Sam4 vtable | path |
|---|---|---|
| ✓ `CSimpleHudElement` family: Health, Armor, Lives, CurrentAmmoL, Points, Time*, FragLimit, GoalLimit, RoundLimit, MyBurden, LTK, Geiger, SeriousSpeed, VehicleAmmo, GameMode (0x14010c580); MyRank (0x1400faf00); Netricsa notifier/skills (0x1400fda30) | 0x1415d8980 (Health), 0x1415c5340 (base), +19 more with `vt+0x98` = 0x14010da40 | icon/text `vt+0xe8`/`+0xf0` → `vt+0x98` (0x14010da40 → dd1a0); bar/background `vt+0x100` → `vt+0xa0` (dceb0 → dd1a0) |
| ✓ `CMissionObjectiveHudElement` (0x1400f5390) | 0x1415e5b18 | title via 0x1400d9d20; lines via 0x1400f5910; **also draws objective markers** (§3.3) |
| ✓ message family (0x1400eff90): Message, ItemPicked, CenterHint, TutorialHint, WeaponModeChange, Wrestling prompt, LTK | 0x1415c81b8, 0x1415e1270, 0x1415fd1d0, 0x1415ff618, 0x141605c98, 0x1415e2c78 | direct 0x1400d9d20 |
| bars (0x1400af8d0): Boss, Boss henchman, Air, Jetpack heat, Content damage timer | 0x1415c2aa8, 0x1415c7930, 0x1415c7c58, 0x1415e1698, 0x1415c8d00 | `vt+0x98` |
| ammo 0x1400ac080, horizontal weapon list 0x1400d4670, power-ups 0x140106c90, Netricsa 0x1400fcab0, keys 0x1400ee540, gadget selection 0x1400c8390, player list 0x140104b40, HUD log 0x1400ddb20, tip 0x140117d70, demo 0x1400b8a30, time recorder 0x140115e70, cut-scene info 0x1400b5f40, hint 0x1400d3a00, game over 0x1400cb410, game phase 0x1400cc7a0, game saving 0x1400cd630, radio chat 0x1401081d0, spectator 0x14010de10, voting 0x14011f740, count-down 0x1400b43d0, fight feedback 0x1400c5d50, multiple items 0x1400f9ec0, SS1 messages 0x14010e8f0, text descramble 0x140111c70, achievement 0x1400aac20, element at 0x140110060 | see `class_vtables` | `vt+0x98`/`vt+0xa0` or direct |
| discussion / subtitles (0x1400bc0d0 → 0x1400bc110 / 0x1400bcbc0) | 0x1415ce640 | `vt+0x98` + 0x1400d99a0 + 2D reset; the line layout was not traced to the end (medium) |

SS4 has no minimap element (no such params class in `HudParams.rsc`). The mission-objective title is at virtual
(320, 177.2), the centre zone of both axes (it is already near the image centre). Its other lines are drawn by 0x1400f5910 (layout-mapped; their position source was
not traced). Top-left items in content are e.g. Time playing (21, 30) and My rank (13.8, 21).

### 3.2 Screen-centred from the drawport box (not inset; sizes use the unit)
Damage direction 0x1400b68a0, explosive indicator 0x1400c5200 and projectiles indicator 0x140107b10 draw rings around
`(W/2, H/2)` with radius `unit × param` (0x1400d9990). Blood spray 0x1400b0a60 and stains 0x1400b1c50 are full
screen. Guided jump 0x1400d2230 reads the safe-area getter itself (0x1400d2603, pos 0x1400d260c).

### 3.3 World-projected (positions from 0x141152ae0 / player projection, not from the layout map)
| element | render (Sam4 / SM) | layout use | recommendation |
|---|---|---|---|
| ✓ `CWeaponCrosshairHudElement` | 0x140120b60 / 0x140121570 (vt 0x1415ff2f0 / 0x1415e1c70) | size only: 0x1400d99a0 → dc8e0 (safe `.y`) | **exclude** (keep native size) |
| ✓ `CObjectHolderHudElement` + aiming indicator 0x1400fe410 | 0x1401015d0 / **0x140101ef0** (vt 0x1415ea738 / 0x1415ccdc8) | `vt+0x98(params+0xa0)` = fallback position when not projecting; size 0x1400d99a0/0x1400d9990 | **exclude** |
| player probe | 0x140105b20 / 0x140106430 | none | nothing to exclude |
| auto-aim 0x1400af130, damage numbers 0x1400b78c0, lock-on targets 0x1400ef110 | SM sig ports not unique | none found | no effect |
| critical hit | 0x1400b48b0 / 0x1400b4a90 | size only: 0x1400d9cc0 → dcfb0 (safe `.y`) | harmless (icon shrinks with the size factor) |
| ✓ objective markers `0x140ab90a0` (SM 0x140ab9540), called by the objective render at **0x1400f5402 / 0x1400f5426** (SM 0x1400f54b2 / 0x1400f54d6), only when `prj_iObjectiveMarkerMode` (0x14224756c) ≠ 0 | – | projects with 0x141152ae0. The "on screen" box is `vt+0x98` of (64, 48) and (576, 432). Outside it the marker is clamped to the box edge with an arrow (0x1400f3080 test, 0x1400f1b90 clamp). Sizes via 0x1400dc930 / 0x1400db300 | default: let it inset, so markers inside the inset box stay at their true per-eye projection and markers outside are pulled to the readable edge. To keep the native box, wrap the two calls (§5.4) |

So the projected points never pass through the layout map. Turning the inset on for the whole HUD would move only
the objective-marker clamp box and the object-holder fallback point, and shrink projected icon sizes by the size factor.

## 4. Alternatives considered

| approach | why not |
|---|---|
| Write `gfx_fSafeAreaX/Y` or `hud_fCustomHudScale` | Persistent cvars (forbidden). X/Y are clamped to ≥ 0.85 and written back (§2) |
| Shrink the drawport box `dp+0x10..+0x1c` during the HUD | Projected elements take the viewport size from the same box (crosshair 0x140478b60 aspect and the 0x141152ae0 viewport, critical hit, objective markers). The GPU viewport is set at drawport push, not from the box. The split-screen branch of 0x1400dd1a0 changes the margins when `x0 ≥ 1` or `x1 < raster/2`. The box would have to be swapped per element, and that is more fragile than §5 |
| Post-multiply the 2D view/ortho (gfx+0xc70, 0x140b94200) | Queued canvas entries are drawn by the flush 0x1400ea890 with a fresh 0x140b94200 per 2D entry (R8 A6). `CGenericHudElement` 0x1400ceb10 overwrites the object matrix. It would need per-element flushes and matrix bookkeeping, and text would scale non-uniformly |
| MinHook on 0x1400dd1a0, `out = c + k(out − c)` | Moves anchors but not unit-scaled offsets, so groups keep native size while their spacing shrinks. Elements that map several points through dd1a0 (rect corners via dceb0 keep the size) become inconsistent. Usable only as a crude fallback |
| Detour the getter 0x140b99060 | 34 callers incl. menus. Possible with a thread-local gate, but the call-site retargets below are narrower and separate positions from sizes |

## 5. Recommended hook

### 5.1 Call sites (all `E8 rel32`; verify the target before patching, fingerprint rule)
Offsets are from the layout-map start (`dd1a0`) and are the same in all four builds. Only the absolute addresses move.

| role | rel. to dd1a0 | Sam4 | bytes | SamSM | bytes | Sam4_U | SamSM_U | target |
|---|---|---|---|---|---|---|---|---|
| **margins (positions)** | **+0xd3** | **0x1400dd273** | `E8 E8 BD AB 00` | **0x1400dd343** | `E8 C8 C3 AB 00` | 0x1400dfbc3 | 0x1400dfc83 | safe_area |
| **inset position (centre)** | **+0x144** | **0x1400dd2e4** | `E8 E7 BD AB 00` | **0x1400dd3b4** | `E8 C7 C3 AB 00` | 0x1400dfc34 | 0x1400dfcf4 | safe_pos |
| size: unit in map | +0x95 | 0x1400dd235 | `E8 26 BE AB 00` | 0x1400dd305 | `E8 06 C4 AB 00` | 0x1400dfb85 | 0x1400dfc45 | safe_area |
| size: dc8e0 | −0x898 | 0x1400dc908 | `E8 53 C7 AB 00` | 0x1400dc9d8 | `E8 33 CD AB 00` | 0x1400df258 | 0x1400df318 | safe_area |
| size: dc930 | −0x834 | 0x1400dc96c | `E8 EF C6 AB 00` | 0x1400dca3c | `E8 CF CC AB 00` | 0x1400df2bc | 0x1400df37c | safe_area |
| size: dceb0 (rect) | −0x25f | 0x1400dcf41 | `E8 1A C1 AB 00` | 0x1400dd011 | `E8 FA C6 AB 00` | 0x1400df891 | 0x1400df951 | safe_area |
| size: dcfb0 ×2 | −0x173, −0x103 | 0x1400dd02d, 0x1400dd09d | `E8 2E C0 AB 00`, `E8 BE BF AB 00` | 0x1400dd0fd, 0x1400dd16d | `E8 0E C6 AB 00`, `E8 9E C5 AB 00` | 0x1400df97d, 0x1400df9ed | 0x1400dfa3d, 0x1400dfaad | safe_area |
| size: dd110 | −0x3f | 0x1400dd161 | `E8 FA BE AB 00` | 0x1400dd231 | `E8 DA C4 AB 00` | 0x1400dfab1 | 0x1400dfb71 | safe_area |

These are the only `E8 → safe_area/safe_pos` in `[dd1a0 − 0xa00, dd1a0 + 0x400]` in each build (scan). The size
sites read only `.y` (`[rsp+0x44]` / `[rsp+0xcc]`), and the margin site reads both.

**Symbols to add** (`docs/re/builds`, all four builds):
* `hud_layout_map`: Sam4 0x1400dd1a0, SM 0x1400dd270, Sam4_U 0x1400dfaf0, SamSM_U 0x1400dfbb0. 64-byte signature,
  unique in all four:
  `48 8B C4 48 89 58 08 48 89 68 10 48 89 70 18 57 48 81 EC A0 00 00 00 49 8B 99 78 1C 00 00 49 8B E8 48 8B FA 48 8B F1 48 85 DB 0F 84 ?? ?? ?? ?? 48 83 3B 00 0F 84 ?? ?? ?? ?? 8B 53 1C 2B 53 14`
  (`sig_at` over 0x360 bytes: 824/824 masked bytes match in SM and both Unrestricted builds).
* `gfx_safe_area`: Sam4 0x140b99060, SM 0x140b99710, Sam4_U 0x140b9ba10, SamSM_U 0x140b9c0b0. 48-byte signature,
  unique in all four:
  `83 3D ?? ?? ?? ?? 00 48 8B C1 75 1A F3 0F 10 05 ?? ?? ?? ?? F3 0F 10 0D ?? ?? ?? ?? F3 0F 11 01 F3 0F 11 49 04 C3 F3 0F 10 0D ?? ?? ?? ?? F3 0F`
* `gfx_safe_pos`: Sam4 0x140b990d0, SM 0x140b99780, Sam4_U 0x140b9ba80, SamSM_U 0x140b9c120 (`port_list` unique 24
  bytes; `sig_at` 84/84). In practice it can be read from the `E8` at `hud_layout_map+0x144`.
* At install, locate the sites as `hud_layout_map + off` for +0xd3/+0x144/+0x95. Locate the six size sites by scanning
  `[hud_layout_map − 0xa00, hud_layout_map)` for `E8` whose target is `gfx_safe_area`. Expect exactly six; otherwise
  install positions only.
* Exclusion renders: `crosshair_render` (exists) and new `objholder_render`: Sam4 0x1401015d0, SM 0x140101ef0
  (`sig_at` 232/232); Unrestricted builds by `port_list`.

### 5.2 Thunks (render/main thread, same thread as the HUD loop)
```cpp
struct Inset { bool on; float s, k, px, py; } g_in;      // set per element (5.3)
using SAFn = float*(__cdecl*)(float* out);              // returns out
SAFn realSafeArea, realSafePos;

float* __cdecl layoutMargins(float* o) {                // retarget of hud_layout_map+0xd3
    realSafeArea(o);                                    // native value (and the engine's own clamp/write-back)
    if (g_in.on) o[0] *= g_in.s, o[1] *= g_in.s;
    return o;
}
float* __cdecl layoutUnit(float* o) {                   // retarget of the 7 size sites
    realSafeArea(o);
    if (g_in.on) o[1] *= g_in.k;                        // only .y is read at these sites
    return o;
}
float* __cdecl layoutPos(float* o) {                    // retarget of hud_layout_map+0x144
    realSafePos(o);
    if (g_in.on && g_in.s < 0.999f) o[0] = g_in.px, o[1] = g_in.py;
    return o;
}
```
`patch::nearJump` + `patch::retargetCall`, the same mechanism as `gateHud()` in `src/se4/hud_vr.cpp`. The `xmm`
registers the callers keep live (`xmm6..xmm13`) are callee-saved in the Win64 ABI, so a C++ thunk is safe. `rax` must
return `o`: the callers read `[rax]` (0x1400dd27b).

### 5.3 Turning it on per element (in the existing `hookedElement`, Sam4 0x1400d6920 / SM 0x1400d6a00)
On the path that today ends in plain `realElement(ctx, e)` (the 2D branch):
```
eye = vrdev::currentEye()
on  = g_mode == 0 && option hud_inset > 0 && eye ∈ {1,2} && ctx[7] && *(int*)ctx[7] == 0     // flat 2D, eye pass
      && vt[0x80/8] != crosshair_render && vt[0x80/8] != objholder_render
if (on) {
   s = hud_inset/100 (default 0.70), k = hud_inset_scale/100 (default = s; 1.0 keeps native sizes)
   // centre the rectangle on this eye's optical axis (asymmetric per-eye FOV); vrdev::eyeFov gives tangents
   xc = -tan(l) / (tan(r) - tan(l));  yc = tan(u) / (tan(u) - tan(d))       // fractions of W, H (y down)
   px = clamp((xc - s/2) / (1 - s), 0, 1);  py = clamp((yc - s/2) / (1 - s), 0, 1)
   g_in = {true, s, k, px, py}
}
realElement(ctx, e)
g_in.on = false
```
* Positions are computed inside the element render and stored as pixels in the canvas queue. The flush 0x1400ea890 does
  not call the layout (no caller of dd1a0/the getter in it), so batching needs no change and no per-element flush.
* Axis centring places the rectangle at the same angles in both eyes. Since both eyes share the vertical FOV and the
  unit depends only on H, an element's pixel offset from the axis is the same in both eyes. It therefore fuses at
  infinity like the rest of the screen-locked HUD. With `px = py = 0.5` (image centre) the HUD would get a small
  vergence error, because the per-eye images are asymmetric.
* `hud_inset_axis=0` can fall back to `px = py = 0.5`.
* While the wheel panel is up, `flatGate` keeps the 3D branch (`*ctx[7] = 1`), so the wheel never sees the inset.

### 5.4 Optional
* **Objective markers with the native box:** retarget `E8 → obj_marker` at 0x1400f5402 / 0x1400f5426 (SM 0x1400f54b2
  / 0x1400f54d6, bytes `E8 99 3C 9C 00` / `E8 75 3C 9C 00`, SM `E8 89 40 9C 00` / `E8 65 40 9C 00`) to a thunk that
  saves `g_in.on`, clears it, calls 0x140ab90a0 (SM 0x140ab9540; `sig_at` 464/464) and restores it.
* **Guided-jump prompt:** retarget 0x1400d2603 (safe_area) and 0x1400d260c (safe_pos) to `layoutMargins`/`layoutPos`
  if it is seen at the lens edge.
* **Centre rings** (damage direction, explosive and projectile indicators) stay at `(W/2, H/2)`. If they should follow
  the axis, that needs per-element work; not recommended.

### 5.5 Expected result (lab eye 1344×1512, s = k = 0.70, axis at x 0.5)
Margins 202 px left/right and 227 px top/bottom, unit 1.64. Health moves from (40, 1418) to about (230, 1219). The
objective title (320, 177.2) is in the centre zone of both axes (177 > 480/3). It stays centred and moves from
y = 756 − 63·2.35 = 608 to y = 756 − 63·1.64 = 653. Crosshair, probe, lock-on, damage numbers and
critical hits are unchanged. Objective markers are unchanged inside the central box (now about 70 % of the image) and
are clamped to its edge outside it.

## 6. Risks and runtime checks
1. **Drawport shape.** The inset math assumes the HUD drawport covers its whole raster (eye RT in two-pass, EYE_TARGETS).
   If `dp.x0 ≥ 1` or `dp.x1 < raster.w/2`, the split-screen branch replaces `px` with 0/1 and multiplies the margin by 1.5.
   Log once per eye: `dp` box, `*dp+0x20/+0x24`, the native SA2/pos values and the thunk outputs.
2. **Native safe area is not 1.** With `0x14224cf38 == 0` the native pair is the SD value 0.85, so the multiplication
   in the thunks gives 0.85·s. Log it. Use absolute values (`o = s`) if the multiplied value is too small.
3. **Readability vs fit.** At unit 2.35 the virtual 640 is already wider than the eye image (portrait drawport), so side
   groups crowd the centre. If `k` is kept at 1 while `s` < ~0.8, left and right bottom groups can overlap (Health box
   ≈ 235 px wide). Default `k = s`; tune both in the lab.
4. **Single-pass stereo:** one HUD pass with `device+0x18 == 0`, so `currentEye()` is 0 and the inset stays off. That
   is acceptable while two-pass is the default.
5. **Objective markers** change behaviour (earlier edge clamping) unless §5.4 is applied. The markers' own projection
   (camera matrices at `*(hud+0xa8)+0x20` indexed by view id) is per-view, and its per-eye correctness in VR was not
   checked here.
6. **Elements that cache layout results outside `vt+0x80`** would not be inset. Only the objective `vt+0x90` uses the
   unit (a size) outside render, and the wheel's `vt+0x40` geometry is unit-based. Positions were found only inside
   render.
7. **Subtitles/discussion** (0x1400bc0d0) position lines through `vt+0x98` plus 0x1400d99a0. They should inset, but
   this was not traced to the draw. Check in the lab with a dialogue.
8. **Menus** call dd1a0 via 0x140094030 (0x1408da690). The flag is only on inside an element render, so they are
   unaffected.
9. Three class names in the table were not unique (`CAutoAimHudElement`, `CRoundLimitHudElement` and `CLTKHudElement`
   each label two vtables). The exclusion list uses render function addresses, not names.
10. SM: the size sites, margin/pos sites, getter and signatures are verified. The SM content's resolution graph and safe
   area defaults were not extracted (expect the same).
