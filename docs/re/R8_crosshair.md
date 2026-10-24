# Gate R8: projected native crosshairs (Sam4.exe 616154, ported to SamSM.exe 628492)

Static analysis only (Ghidra project `sam4`, raw disassembly for SamSM, Ghidra project `fusion` for the design
reference). No game, Wine, Proton or VR runtime was run. Conventions follow R3: "player" is a
`CPlayerActorPuppetEntity`, "vrd" is `CPlayerVRData`, `vt+0xNN` is a vtable slot. Offsets come from reflection
records (`tools/meta_members.py`, `tools/meta_offset.py`) or from the decompiled code at the cited address.
Anything else is marked unknown. Private decompiles: `lab/r8a.c` … `lab/r8i.c`, `lab/r8f.c` (Fusion).

New read-only tools: `tools/vtable_slot.py` (vtable starts from constructor stores, `of <fn>` lists every
vtable and slot holding a function, `slots <vt> <off>…`), `tools/meta_offset.py` (member record by byte offset,
the reverse of `meta_members.py`), `tools/cname_globals.py` (string of a static CName global, e.g. ray-cast
categories), `tools/elf_riprefs.py` (`cvar_readers.py` for the Fusion ELF).

## 0. Summary

* The crosshair is the HUD element **`CWeaponCrosshairHudElement`** (vtable 0x1415ff2f0, render vt+0x80 =
  **0x140120b60**). It already computes its position by **projecting the aim target `player+0x2188`** through
  the player camera (`player vt+0xdf8`) and the player projection (0x140478b60), and it already draws at a
  **stereo depth equal to the target distance `player+0x2194`** (drawport depth setter 0x140c6c040).
* The projection is switched off by an unnamed predicate, called **F** here. F is identical-code-folded into the
  shared `mov eax,1; ret` stub **0x1400472e0** (SM 0x140047350), so it always returns 1 in the shipped build:
  the crosshair is snapped back to the screen centre, and the aim writer 0x1404a3670 casts the aim ray from the
  camera (the head in VR). With F = 0 the engine's own code:
  1. casts the aim ray from the **weapon shoot placement** (`player vt+0x340` mode 1 = `+0xf3c`, which is
     `weapon vt+0x9b0` = the **barrel placement in VR**), writing `+0x2188` (hit point), `+0x2194` (distance)
     and `+0x21e0` (hit entity: colour coding, health bar), and
  2. draws the crosshair and the dispersion ring at the projection of that hit point.
* **Firing in VR already follows the barrel**, for every fire path found: the fireable path (bullets, pellets,
  beams and `CWeaponMode` fire) gets its direction from `weapon vt+0x9b0`, and that function returns
  `GetBarrelPlacement` when the owner has vrd (0x1402d57e0). Projectile launch 0x140475be0 skips the
  "turn toward +0x2188" step and the spread step when vrd exists. So the hit point of the **barrel ray** is
  where the shots land, apart from spread.
* Fusion VR does not project its crosshair. It draws a **3D laser-sight beam** from the barrel (weapon render
  0x17b7460, `prj_bRenderVRLaserSight`). Its HUD crosshair stays at the screen centre at the target depth. SS4
  keeps the same laser (0x1402e0390). The F-gated projection exists only in the SS4-era code.
* Recommendation: make F return 0 for the local VR player at the **six call sites** listed in section 8. Do not
  patch the shared stub. Then handle the per-eye view (section 6), and draw the second crosshair for dual
  wield with the same native helpers.

## 1. Where the crosshair is drawn

### Element and entry points
| item | Sam4 616154 | SamSM 628492 | evidence |
|---|---|---|---|
| class record `CWeaponCrosshairHudElement` : `CHudElement`, params `CWeaponCrosshairHudElementParams` ("Weapon crosshair") | name 0x1415ff290, factory 0x140120180 | — | record 0x1415ff150; factory stores the vtable at 0x140120197 |
| vtable | **0x1415ff2f0** | **0x1415e1c70** | `vtable_slot.py of 0x140120b60` → slot +0x80 |
| render (vt+0x80) | **0x140120b60** | **0x140121570** | vtable slot; call layout of the two builds matches 1:1 |
| dispersion-ring draw | 0x140121410 | 0x140121e10 | called from render (0x14012115a / 0x140121b2d) |
| supplements (zero-damage overlay) | 0x140121ec0 | 0x1401228c0 | called after the main draw |
| health-bar-over-target (colour option 2) | 0x140122250 | 0x140122c50 | |
| HUD render (calls every element) | 0x1400d7be0 → per element 0x1400d6920 → element vt+0x80 | 0x1400d7cc0 | |
| HUD render callers | `CPlayerController` vt+0x28 = 0x140415200 (×3), player vt+0xe10 = 0x14048f760 | 0x140415ba1/0x1404150f0, 0x14048fdda | `callers.py` |

### Reads (cvars, params, player state)
| what | where read | notes |
|---|---|---|
| `hud_bShowCrosshair` 0x142247280 (SM 0x142228180) | show predicate **0x1404947c0** (SM 0x140494eb0), sole reader | also requires alive, not flag `+0x7a4` bit 25, not 0x14047edb0, rider/weapon checks, then `weapon vt+0x7e8` (base = the return-1 stub) |
| `hud_fCrosshairScale` 0x142247554 (SM 0x142228444) | render 0x1401210c3.. | scale = `0x1400d99a0(hud, dp) / hud_fCustomHudScale(0x142247550) * hud_fCrosshairScale`; 0x1400d99a0 = drawport height × HUD-params factor / 1080 |
| `cgtp_pcpCrosshairParams` (`CCommonGameTitleParams+0x60`) | `hud->+0xa8 (params) ->+0x3c0 (cached game-title params, filled by 0x140051470) ->+0x60` | `CCrosshairParams`: `cp_aptxCrosshairs` +0x60 (count +0x68), `cp_grCrosshairColorCoding` +0x70, `cp_fCrosshairColorCodeWidth` +0x80, `cp_aptxDispersionCrosshairs` +0x88 (count +0x90), `cp_ptxZeroDamageCrosshair` +0x98, `cp_ptxLockOnCrosshair` +0xa0, `cp_fHealthDecaySpeed` +0xa8, `cp_fBlockFadeSpeed` +0xb0, `cp_cslSupplementDisplay` +0xc0 (reflection) |
| crosshair / dispersion style index | `0x140478950(player)` → object +0x58 (crosshair), +0x60 (dispersion); default 4 with no profile | `po_iGameOptionCrosshair` is `CProfileOptions+0x28` and `po_iGameOptionDispersionCrosshair` is +0x30, so the object probably embeds `CProfileOptions` at +0x30 (consistent, not proven). `prj_iGameOptionCrosshair`/`…DispersionCrosshair` (0x1422739d4/d8) are read only by the options menu 0x1408632b0 |
| colour coding | `plr_iHealthBarOption` 0x14224886c (SM 0x14222942c) == 1 | colour = gradient `0x140c44c00(cp+0x70)` at the quantised ratio health(0x1402a5f30) / max(0x1402a7ac0) / `cp+0x80`, for the hit entity `handle(player+0x21e0)` if it is a puppet (class 0x1422817a0) and alive (0x14029daf0). Option 2 draws a health bar over the target instead (0x140122250) |
| lock-on indicator | `cp_ptxLockOnCrosshair`, faded for the unnamed global 0x142247ff0 seconds since `player+0x2240` | +0x2240 is stamped by the aim writer (0x1404a3670) when the hit target is alive and 0x140480510(player) |
| zero-damage indicator | `cp_ptxZeroDamageCrosshair`, from `player+0x2230/+0x2238` timestamps, colour 0xffbfbfbf | only when `cp_cslSupplementDisplay == 0` (on crosshair) and 0x140472310(player) |
| skip conditions | `player+0xfa8` bit 13 (consumed and cleared each frame) or weapon-wheel bit `+0x269c` bit 0; no right weapon `+0xf90` and no ride `+0x75c`; empty crosshair list; `+0x2568` (0x14049c010) set | render 0x140120b60 |

### How the screen position is computed
Render 0x140120b60:
1. `player vt+0xdf8(&cam, flag)` (0x1402a42a0) → `vt+0x340` mode 0 (or 2 when `+0x1010` bit 26 / 0x140ba6da0), or the
   third-person camera 0x1402a9070. With vrd, mode 0 is the **head world placement** 0x14043cb50 (R3 §3).
   `0x1411467c0` turns it into a 3×4 view matrix.
2. Projection `0x140478b60(player, out, dp)` (aspect = dp width/height) → **0x140478a80**(player, out, aspect) (SM 0x140479190 → 0x1404790b0): if the VR device exists and `device+0x18` (eye being
   rendered) ≠ 0, it is **device slot 0x70 for that eye** (near/far 0x14224d388/0x14224d38c). Otherwise it is the
   player FOV (0x140477aa0 × 0x140477e60, clamped 5..135°) through 0x141111460.
3. Target `T = player+0x2188` (Vector3, `+0x2190` = z); `D = player+0x2194`.
4. No weapon (riding only): `pos = (w/2, h/2)`; **if F(player) == 0** (call 0x1401211e8, SM 0x140121bca),
   `pos = project(T)` with **0x141152ae0**(view3x4, proj4x4, &point, &viewportWH, &outXY, &outVisible)
   (SM 0x141153340; standard clip-space projection, y down).
5. With a weapon (`+0xf90`), 0x140121410 builds the dispersion box. Ring half-sizes are
   `tan(a) * max(D - |P0 - P1|, 0.01)`, where `a` = `max(weapon vt+0x9f0, other-hand weapon vt+0x9f0)`
   (0x1402dc8c0 = the other hand's weapon), P0 = mode-0 view position and P1 = mode-1 view position. The four
   corner points `T ± right·sx ± up·sy` (axes from the **mode-1 orientation**, `+0xf3c`) and T itself are
   projected with 0x141152ae0. **If F(player) ≠ 0** (call 0x1401218d4, SM 0x1401222d4), the whole set is
   shifted so that T lands on the screen centre. The box is drawn with 0x140120300 (nine-slice textures,
   texture vt+600 ≠ 0) or one quad at the projected T.
6. Depth: `0x140c6c040(dp, max(D, 0.5))` (0x1401212a7 and in 0x140121410) stores the drawport stereo depth at
   `dp+0x1d90`; it is reset to 1.0 at the end (0x1401213a3). The HUD baseline is
   `hud_fStereoDepth` (0x14224755c, SM 0x14222844c), clamped to 0.1..100 and set at 0x1400d7dbc.
7. Draw: 0x1400e8d20(hud+0xb0 canvas, dp, texture, rect, colour). The black outline is eight copies offset by
   ±1 px.

So **the native path that projects a 3D point to the crosshair position already exists** (step 4/5 with F = 0).
The same predicate gates the target projection in `CObjectHolderHudElement` vt+0x80 0x1401015d0 (call
0x140102626, SM 0x140102f26), which is the gadget/held-object aim indicator. The aiming indicator 0x1400fe410
always projects its target box and sets the depth to the target distance.

### Predicate F
* The six direct calls of 0x1400472e0 that take the player and feed this logic are listed in section 8. They
  are the crosshair (×2), the object-holder HUD (×1), the aim writer (×2) and the desktop branch of the shoot
  placement (0x1402d5c35). The stub has 31 direct callers and over 10 000 data pointers (`callers.py`), because
  every `return 1` function in the program folds into it. **Patch the call sites, never the stub.**
* The name and source of F are unknown. Fusion has no equivalent: its crosshair 0x1a59380 recentres
  unconditionally.

## 2. Dispersion (spread) size

| `weapon vt+0x9f0` | value (half-angle pair, used as `tan(a)`) | vtables |
|---|---|---|
| 0x1402d8570 (CWeaponEntity base) | (0, 0) | 0x141716048 and six others (`vtable_slot.py of`) |
| **0x14025c520** (CBaseWeaponEntity) | if `weapon+0xa8` params are `CBaseScatterWeaponParams` (class 0x142281188): `bswp_fScatterRadiusMax (+0x2dc) / bswp_fScatterDistance (+0x2d4)`, else 0 | base 0x1416b8888 and 25 weapon vtables |
| **0x140763a70** (CAimableWeaponEntity: AssaultRifle 0x141a8ef80, Pistol 0x141abfd38) | `awp_fNonAimedScatterRadiusAt100 (params +0x320) / 100 × (1 - en_fAimRatio (+0x410))` | 3 vtables |

So the ring depends only on weapon params, `en_fAimRatio`, the target distance `+0x2194`, the camera-to-shoot-origin
offset `|P0 - P1|`, the mode-1 orientation and the projection. With dual wield it shows the larger spread of
the two hands. The same SM reflection offsets apply (`en_fAimRatio` +0x410, `bswp_*` +0x2d4/+0x2dc, `awp_*`
+0x320). With F = 0 in VR, D is measured from the barrel, so the `- |P0 - P1|` term (head-to-barrel,
about 0.5 m) makes the ring slightly too small close up. The error is `tan(a) × |P0 - P1|`, negligible at range.

## 3. Shot direction per fire path

`GetBarrelPlacement` native **0x1402d7220** (SM 0x1402d6e40, 51 direct callers in both): with `+0x1f0`
(`en_bOwnerPlayingInVR`) set it uses the model barrel 0x1402d7360 + 0x1402d6910 (hand-placed model, R3).
**Shoot placement** `weapon vt+0x9b0` (base **0x1402d57a0**, SM 0x1402d5400; thunk 0x1402d5770):
`if (owner+0x25c8 /*SM +0x25d0*/ != 0) return GetBarrelPlacement(...)` (0x1402d57e0). Otherwise (desktop) the
direction is the view forward, the origin is the barrel projected onto the view line, then auto-aim
(`weapon vt+0xa68`, target `+0x2174`, `vt+0x350`) or the `+0x21a8/+0x21ac` correction.

| fire path | functions (Sam4) | direction source with vrd |
|---|---|---|
| **Fireable object** (`CFireableBullet/Projectile/Beam` via 0x14076a120): hitscan rifles, shotguns (pellets), minigun, sniper, Tommy gun, pistol | `weapon vt+0xb28` overrides: AssaultRifle 0x1407658f0, DoubleShotgun 0x140773120, MiniGun 0x140791490, Pistol 0x1407a3f40, SingleShotgun 0x1407ab580, Sniper 0x1407beee0, TommyGun 0x1407cee50, unnamed vt 0x141b6a968 0x140837b70/0x140837d80 | 0x14076a120(fireable, world, **shoot placement = vt+0x9b0**, barrel placement, spread): orientation = shoot placement ∘ spread, origin = barrel position → **barrel** |
| **`CWeaponMode` family** (SS4 weapon modes, "CWeaponMode::OnRecoilEnd") | fire 0x140a6c0c0 (vt 0x141c81188 +0x250) from 0x140a6baf0 | shoot placement = 0x140a76410 = `weapon vt+0x9b0` → `fireable vt+0x18` or 0x140a5d580 → projectile launch → **barrel** |
| **Projectile launchers** (RocketLauncher 0x1407a4bd0, Cannon 0x14076d4a0, GrenadeLauncher 0x140782830 call 0x140475be0 directly; Laser 0x14078d330, AutoShotgun 0x140767080, DoubleShotgun 0x1407731aa, 0x14077a51b and `CWeaponMode` 0x140a5d580/0x140a5d71c go through the wrapper 0x140475b10) | barrel placement (or the vt+0x9b0 placement for `CWeaponMode`) copied whole into the launch | launch **0x140475be0** (SM 0x140476200): it turns toward `+0x2188` and adds spread only `if (player+0x25c8 == 0)`, so with vrd the projectile keeps the **barrel** orientation and gets **no spread**. Quirks: if the projectile params have `+0xac > 0` and `+0x2194 < 5`, the spawn point moves to the aim-ray origin `+0x2254` (the head with F = 1, the barrel with F = 0); a blocked body→spawn segment (0x1401acbc0) pulls the spawn back |
| `CAutoShotgunWeaponEntity` vt+0x9b0 0x140766c70 | | if `*(*(weapon+0x410))` ≠ 0 (`en_pszhZoomHandler`), uses **player view mode 0 = head**, even in VR; otherwise barrel. **Exception to check at runtime** |
| `CSniperWeaponEntity` vt+0x9b0 0x1407bdd10 | | view only when `+0x1f0 == 0` and zoomed → barrel in VR |
| vt+0x9b0 0x140799eb0 (vt 0x141abcd20, class unknown) | | barrel when `+0x1f0`; view otherwise |
| vt+0x9b0 0x14077d650 (vts 0x141a9d040/0x141a9f660/0x141aa44b0, class unknown) | | base result, then optional override object `+0x320 vt+0x60` (unknown) |
| barrel used only for effects/visuals | 0x14025dc60 (muzzle flash, base vt+0xb28), 0x14025a8f0 (vt+0xb40), 0x14025e2b0, 0x1405632f0 (`CStreamTrailRenderableForWeapon`), 0x140943790 (`CWeaponFlamerTrailManager`), 0x14055d660, 0x1402d1ab0 (script) | n/a |
| grappling hook 0x14023d990, sight/laser model 0x14023ad60 | read `+0x2188/+0x2254/+0x2260` and the barrel | not traced in detail |
| melee (`CVRMeleeWeaponEntity` 0x1407d5410), hammer 0x1407843d0, other unclassified callers (0x14076e1f0, 0x14077a200/0x14077a490, 0x14078fd60, 0x140791800/0x140791b10, 0x1407cd0d0/0x1407cda80/0x1407cdd90, 0x140792a80, 0x140a65180, 0x140a6f5a0, 0x140a71140, 0x140a74400, 0x140a643f0, 0x140a67b70) | | not classified (unknown) |

Conclusion: for the bullet, shotgun, beam and projectile paths **the shot already goes along the barrel ray in
VR**. `+0x2188` is not used as the shot direction there. Spread differs: fireables apply it, while VR projectiles
skip it.

## 4. Aim writer and the native ray cast

Aim writer **0x1404a3670** (SM **0x1404a3c50**; the call sequences of both builds align 1:1):
* `A = vt+0x340 mode 0/2` (camera; head with vrd), `B = vt+0x340 mode 1` = `+0xf3c`. `+0xf3c` is rebuilt in
  0x14046cd40 through `vt+0x11c8` = 0x1404795a0 mode 1. With a right weapon whose `vt+0x908` ≠ 0 (base = the
  return-1 stub) that is `0x1402d5770` = `weapon vt+0x9b0` = **the barrel placement in VR**.
* Ray = B if `!IsTalos && F(player) == 0` (call **0x1404a3a23**, SM 0x1404a4003), else A
  (`jne 0x1404a3a91` after the call).
* `0x1401acbc0(player, &origin, &res)` checks the segment from the body (x, z at view height) to the origin with
  category `bullet_no_shootable_ghost` (0x1422770a0). If it is blocked and F == 0 (call **0x1404a3e06**, SM
  0x1404a43e6), the target becomes the origin with distance 0.
* Main cast: `0x1401a4280(res)` reset, `res+0 = handle(player)`, `0x1401a5030(res, 0x1404a21a0)` filter callback,
  **`0x1401a4f20(res, world, &origin, &dir, category, "", maxDist)`**. The category is `camera_aim_ray`
  (0x142286cc0) for players or `bot_aim_ray` (0x142286cb8) when `vt+0x10b0`. maxDist = `0x14019bcf0(player)+0x370`.
  Then 0x14046e300(player, &res).
* Writes: `+0x2188/+0x2190` = res+0x28/+0x30 (hit point), **`+0x2194` = res+0x70 (distance)**, `+0x21c8` normal
  (res+0x34..), `+0x21c0` = res+0x50 (ref-counted), `+0x2198` = handle(res+0x60), `+0x21e0` = res+0x08 (hit
  entity), `+0x2254` origin, `+0x2260` direction.
* A second cast with `use_aim_ray` (0x142286cb0), 1e6 m, from A, drives the use/interaction prompts (0x1404a2530).

**Ray-cast API** (pinned from the call sites 0x1404a3bcd / 0x1404a3f24 (SM 0x1404a41ad / 0x1404a4504), 0x14043a1cd, 0x140440cdf, the laser 0x1402e0390 and the body of 0x1401a4f20):
```
void RayCast(CRayCastResult* res, CWorld* world /* player+0x30 */, const Vector3f* origin,
             const Vector3f* dir /* unit */, CName category /* 8-byte value */, const char* ""/*0x141567715*/,
             float maxDist);                    // Sam4 0x1401a4f20, SM 0x1401a5b00
void SegmentCast(res, world, const Vector3f* from, const Vector3f* to, CName category, const char*);
                                                // Sam4 0x1401a4f70
res: ctor 0x1401a3f80 (SM 0x1401a4b60); reset 0x1401a4280 (SM 0x1401a4e60);
     set filter callback 0x1401a5030 (SM 0x1401a5c10); skip current hit and continue 0x1401a4560;
     destroy: 0x140c4ab20(res+0xa8) and release res+0x50 with 0x1410c6b00.
     +0x00 hen ignore (in), +0x08 hen hit entity, +0x10 origin (in), +0x28 hit point, +0x34 normal,
     +0x50 ref ptr, +0x60 hit object, +0x68 index, +0x70 distance, +0x74 = 3 (ray), +0x78 dir, +0x88 category.
     Stack size used by callers: 0x1f0.
```
0x1401acbc0 is not a general ray helper. It is the "is the origin reachable from the body" check (it returns when
`world+0x210 > 0x14224d62c` and skips the player's own vehicle/attachments, class 0x1422884a8).

The VR laser 0x1402e0390 uses the same cast from the weapon model attachment `"Laser"` (0x142282670,
`0x1402d60e0`) with category `weapon vt+0xab0` (base 0x1402d6010 → `use_aim_ray` 0x142282668), 1000 m, gated by
`prj_bRenderVRLaserSight` 0x142247fec / mixed-reality cvars and `weapon+0x1f0`.

## 5. The VR per-hand rays (CPlayerVRData)

* vrd step 0x1404399c0 → **0x140440950** casts one ray per hand every step (0x140440cdf / 0x140440ec9) and
  stores, per hand at `vrd + hand*0xd0`: hit point +0x5c, distance +0x78, entity +0x80, usable entity +0x7c
  (script getters `CPlayerVRData::GetUsableEntity{Left,Right}{,HitPoint,HitNormal,HitDistance}` 0x140437390..,
  `GetHitEntity*` 0x140437660/0x1404376e0). The laser-pointer objects vrd+0x3d8/+0x4e0 cast `teleport_aim_ray`
  (0x142286190) separately.
* **0x14043dcb0** (SM 0x14043de00) copies that hand entry into `player+0x2188/+0x2190/+0x2194/+0x21e0/+0x21f8`
  and sets `player+0x2504 = (hand == 0)`. It then calls 0x140480810 or 0x140489100 (use action). It is called
  only from the vrd step's per-hand button branch (0x14043a4b4 / 0x14043a516). That branch needs the button state
  for index vrd+0x2a4 / +0x294 to have `(state & 5) == 1` and 0x14047fab0 to be 0. So it is a **use-button
  path, not a per-frame aim source**. When it runs, it overwrites the aim writer's target until the next
  aim-writer run. Whether those button indices are bound to trigger or grip is set in vrd init 0x14043e330
  (R3) and is unknown here. It does not replace the F change, but the hand hit point it uses is a ready-made
  native per-hand ray result (from the controller, not the barrel).

## 6. Stereo: is the HUD drawn once or per eye?

* The 2D flush **0x140c6c070** builds the 2D constant buffer. When `0x140b6b3a0()` (device present,
  `device+0x1c` ≠ 0 and `device+0x18` == 0, i.e. single-pass stereo) it writes the **eye-1 matrix and the eye-2
  x row** (0x140b623e0 → 0x140b62450, per-eye object-view-projection cached at dp+0x15c0/+0x1640, eye views
  dp+0xe90/+0xef0) and the **stereo depth `dp+0x1d90` at +0x58** (0x140c6c1bd). It is one draw for both eyes; the
  per-eye shift happens in the 2D vertex shader. The shader formula was not inspected (unknown whether the depth
  is metres along the view axis or along the ray).
* In two-pass mode (`device+0x18` = 1/2, R1 / EYE_TARGETS: 0x1401dc520 sets it per eye) the flush uses one matrix
  and the HUD runs inside each eye's view render. **That the HUD render is reached once per eye there is
  inferred, not traced**: 0x1401dc520 → per-eye render callback → `CPlayerController vt+0x28` 0x140415200 →
  0x1400d7be0 was not followed through the callback. In that mode the crosshair projection (0x140478a80) already
  uses that eye's slot-0x70 projection, but the view from `vt+0xdf8` is the centre head placement **without the
  slot-0x78 eye offset**, so the crosshair has no parallax.
* The `device+0x08` (cinema/2D) branch of 0x1400d7be0 places the HUD on a lazily head-following panel
  (`hud+0x90` quat, thresholds 0x142247540/544/548/54c) and sets eye matrices itself. It does not apply to
  immersive play.
* A crosshair drawn into an OpenXR **quad layer** loses all of this. It must be drawn into the eye images by the
  native 2D path.

## 7. Fusion 2017 VR (design reference)

* Crosshair draw (player method) **0x1a59380**: always the screen centre (`w/2,h/2`). The dispersion ring
  uses the same formula as SS4 (`tan(a) × (D − |P0 − P1|)`, `weapon vt+0x898` angles, `vt+0x280` modes 0/1,
  projected with 0x22c81a0), then **recentres unconditionally**. There is no F. Depth `0x1fb64b0(max(player+0x1e64, 0.5))`.
  Scale `hud_fCrosshairScale` 0x39684a8 (reader 0x1a596f0).
* Hand aim feedback: weapon render **0x17b7460** draws a **laser-sight beam model** (`weapon+0x220`) from the
  barrel (0x17b6c90) to a 1000 m ray hit (0xebff80), gated by `prj_bRenderVRLaserSight` 0x3968d40 and
  `vr_bSkipMixedRealityLaserSightRendering` 0x396cb94 (readers 0x17b785d/0x17b786b). That is a 3D beam, not a
  projected crosshair and not a 3D crosshair model.
* Aiming indicator 0x1016810: projects the target box and sets depth = target distance, like SS4 0x1400fe410.
* `hud_fStereoDepth` 0x39684a0 is read only by the HUD render 0xff9990, the same as SS4.

## 8. Recommended hooks

**A. Turn on the native weapon-ray crosshair (primary).** Redirect these `call rel32` sites to a payload thunk
that returns 0 when the argument is the local player and that player has vrd and a VR session is active, and
otherwise returns 1:

| call site (5-byte E8 to the return-1 stub) | Sam4 0x1400472e0 | SamSM 0x140047350 | effect of 0 |
|---|---|---|---|
| aim writer, ray source | **0x1404a3a23** | **0x1404a4003** | aim ray from `+0xf3c` = barrel (vt+0x9b0) |
| aim writer, blocked fallback | **0x1404a3e06** | **0x1404a43e6** | blocked body→barrel → target = barrel, distance 0 |
| crosshair, no-weapon branch | **0x1401211e8** | **0x140121bca** | position = project(+0x2188) |
| crosshair, dispersion branch | **0x1401218d4** | **0x1401222d4** | ring stays at the projected target |
| object-holder HUD | 0x140102626 | 0x140102f26 | gadget indicator projects its target |
| shoot placement, desktop branch | 0x1402d5c35 | 0x1402d5895 | not reached with vrd (returns at 0x1402d57e0); leave alone |

Each site must be checked for `E8 rel32 → stub` before patching (fingerprint rule). Only the local VR player gets
0; desktop peers and bots keep 1. Result: `+0x2188/+0x2194/+0x21e0` come from the right-hand barrel, so colour
coding, the health bar, lock-on timing and use prompts follow the gun. Shots already follow the barrel
(section 3), so the crosshair sits on the impact point, give or take spread. Watch: the use-prompt cast and
the use path (0x14043dcb0) are separate and stay hand/head based. Projectiles with `+0xac > 0` within 5 m now
spawn at the barrel instead of the head (better).

**B. Make it depth-correct in both eyes.**
* Single-pass (`device+0x1c` ≠ 0, `device+0x18` == 0): the native depth (`max(+0x2194, 0.5)` → dp+0x1d90) is
  already applied by the 2D shader. The projection in that mode is the **player FOV** (0x140478a80 falls back
  when `device+0x18 == 0`). Hook **0x140478a80** (SM 0x1404790b0) while the crosshair
  element renders, so that it returns the centre-eye HMD projection (union frustum 0x140b6a8f0 or the mod's own
  matrix). Then the 2D position matches the HMD view. The shader's depth semantics must be confirmed at runtime.
* Two-pass (`device+0x18` = eye): detour **0x1402a42a0** (`player vt+0xdf8`), only while a thread-local flag set
  by a wrapper around crosshair vt+0x80 (0x140120b60 / SM 0x140121570) is on, so that it returns
  `head ∘ slot 0x78(eye)` instead of the head. The projection is already per eye. Set the stereo depth to 1.0
  (no shader shift) in this mode.
* Both cases need the element drawn into the eye images, not into a HUD quad layer.

**C. Dual wield (one crosshair per weapon).** The element draws one target. In a wrapper around vt+0x80, after
the original call, draw the left crosshair with the native helpers and mod-owned data only (no writes to player
state from the render path): left-barrel ray with **0x1401a4f20** (`camera_aim_ray` 0x142286cc0, SM
0x142263e40), ring from `left weapon vt+0x9f0`, texture `cp_aptxDispersionCrosshairs[style]` /
`cp_aptxCrosshairs[style]`, colour 0x140c44c00(`cp+0x70`), projection 0x141152ae0, depth 0x140c6c040, quad
0x1400e8d20 / box 0x140120300. The native ring already takes the max spread of both hands; with two crosshairs
the mod should pass each hand's own value.

**D. Alternative without F (fallback).** Run the barrel ray cast yourself on the simulation thread after the aim
writer, write `+0x2188/+0x2190/+0x2194/+0x21e0` only, and redirect just the two crosshair call sites. This keeps
the head-based `+0x2254/+0x2260` (`GetAimRay` 0x1404639e0, grappling hook, sight model) but duplicates native
work. Prefer A.

**E. Laser.** Leave `prj_bRenderVRLaserSight` off (PLAN: no lasers). 0x1402e0390 draws nothing then.

### SamSM 628492 port summary
Player fields shift by +8 after the weapon handles: `+0xf98/+0xf9c` right/left weapon (reflection),
`+0xfb0` flags, `+0x2190/+0x2198` target, `+0x219c` distance, `+0x21e8` hit entity, `+0x225c/+0x2268` aim ray,
`+0x26a4` wheel bit, `+0x25d0` vrd (R3). Weapon `+0x1f0`, `+0x410`, params offsets are unchanged. Functions:
crosshair vt 0x1415e1c70 / render 0x140121570, ring 0x140121e10, show 0x140494eb0, project 0x141153340, set depth
0x140c6c5c0, player projection 0x140479190, HUD render 0x1400d7cc0, aim writer 0x1404a3c50, ray cast
0x1401a5b00, result ctor 0x1401a4b60, barrel 0x1402d6e40, shoot placement 0x1402d5400, projectile launch
0x140476200, vrd hand copy 0x14043de00, `camera_aim_ray` 0x142263e40, `bot_aim_ray` 0x142263e38. cvars:
`hud_fCrosshairScale` 0x142228444, `hud_fStereoDepth` 0x14222844c, `hud_bShowCrosshair` 0x142228180,
`hud_fCustomHudScale` 0x142228440, `plr_iHealthBarOption` 0x14222942c, `prj_bRenderVRLaserSight` 0x142228c3c.
SM addresses were found by aligning call sequences and field offsets with Sam4 and checking prologues. They are
not yet signature-verified in `docs/re/builds/signatures.json`, and the Unrestricted builds are not ported.

## Unknowns and runtime checks
1. The meaning and name of F. Check that only the local VR player sees changed behaviour.
2. HUD render per eye in two-pass mode, and the 2D shader's depth formula: log `device+0x18`, `+0x1c` and
   `dp+0x1d90` inside the crosshair vt+0x80 wrapper.
3. Whether `+0xf3c` is fresh in the same step as the aim writer (0x14046cd40 runs `vt+0x11c8` modes 0/1; the call
   order relative to 0x1404a3670 was not traced).
4. AutoShotgun zoom-handler branch (0x140766c70) shoots from the head in VR. The unnamed classes behind
   0x140799eb0 / 0x14077d650 and the unclassified barrel callers are unknown.
5. Render-thread vs simulation-thread for HUD rendering (needed for C: the wrapper must not write gameplay state).
6. Which vrd button indices drive 0x14043dcb0 (use path) on our device mapping.

## R8 addendum: crosshair not drawn in VR (2026-10-24)

Static analysis only (disassembly of Sam4 616154 and SamSM 628492, the existing `lab/r8a.c`/`r8b.c`/`r8d.c`/
`r8e.c` decompiles, and the lab logs of the 2026-10-24 Pompeii run). The game was not run. Symptom: the five F
redirects install (log lines at t=0.122..0.124), the local player has vrd (`CPlayerVRData ... (local 1)`), the
weapon wheel renders, but no crosshair is drawn anywhere.

### A1. Cause: the show predicate hides the crosshair for any player with vrd (high confidence)
The show predicate **0x1404947c0** (SM **0x140494eb0**) is the only reader of `hud_bShowCrosshair`. After the alive,
net-flag, bot-view, use-state and terminal checks it tests three player pointers and returns 0 if any is set:
```
Sam4 0x140494844  48 83 BB 50 21 00 00 00  0F 85 ..       cmp qword [rbx+0x2150],0 ; jne ret0
     0x140494852  48 83 BB 58 21 00 00 00  0F 85 ..       cmp qword [rbx+0x2158],0 ; jne ret0
     0x140494860  48 83 BB C8 25 00 00 00  0F 85 AF 00 00 00   cmp qword [rbx+0x25c8],0 ; jne 0x14049491d (xor eax,eax; ret)
SamSM 0x140494f50 48 83 BB D0 25 00 00 00  0F 85 AF 00 00 00   same test on +0x25d0
```
`+0x25c8` (SM `+0x25d0`) is the vrd pointer (R3; the mod's own log: `vrd offset 0x25c8`). So **the vanilla engine never
draws the HUD crosshair for a VR player**. In VR the weapon laser sight (0x1402e0390) replaces it. Section 1 of this
document listed this predicate as "rider/weapon checks" and missed the vrd test.

The predicate has exactly two callers (`callers.py`), and both are in the crosshair element:
| caller | Sam4 | SamSM | gates |
|---|---|---|---|
| render 0x140120b60, no-weapon/ride branch | 0x1401211b0 | 0x140121b92 | the whole quad and its outline (the F site 0x1401211e8 is inside this `if`) |
| ring draw 0x140121410 (called for every held weapon) | 0x140121aa0 | 0x1401224a0 | crosshair texture or nine-slice dispersion box and outline (the F site 0x1401218d4 runs before, but its result is only drawn inside this `if`) |

Only the zero-damage supplement (0x140121ec0) and the lock-on fade (`cp_ptxLockOnCrosshair`) are drawn outside the
gate, and they are rarely visible. This explains why nothing appears at the screen centre or at the target, and why
the F redirects alone change nothing visible. The F redirects themselves are correct, see A3.

The other early-outs were checked and none is VR-specific:
* render: `en_ulPlayerPuppetFlags` (+0xfa8) bit 13, set only by two event handlers (0x1405b1687, 0x1405ea6fd, event
  class 0x142289fd0) and cleared every frame by the render; the weapon-wheel bit `+0x269c` bit 0 (hidden while the
  wheel is open, as intended); `+0x2568` (0x14049c010, two writers outside VR code); no right weapon and no ride;
  empty `cp_aptxCrosshairs`.
* predicate: `+0x7a4` (`en_pnfNetFlags`) bit 25, 0x14047edb0 (bot view type), 0x140480470 (use/terminal state),
  0x140153340 (game-mode object vt+0xa0), `+0x2150`/`+0x2158` (both written together by 0x140464fb0, plus a few non-VR
  writers; not reflected).
* weapon `vt+0x7e8` ("weapon wants a crosshair"): 28 weapon vtables use the return-1 stub. Overrides:
  0x140052890 returns 0 (4 vtables, crosshair-less weapons); **0x140764550** (AssaultRifle 0x141a8ef80, Pistol
  0x141abfd38 and 0x141a8c5b0) returns 0 when `en_fAimRatio` (+0x410) ≥ 1.0 and the owner is not in third person
  (`vt+0xdf0`); 0x140766850 (AutoShotgun) and 0x1407bfed0 (Sniper) return 0 while zoomed; 0x140788ed0 and
  0x1407a1d90 are weapon-specific. None tests `+0x1f0` or vrd. Runtime check: if the two-handed grip ever drives
  `en_fAimRatio` to 1, rifles and pistols hide the crosshair while aimed (the shipped design).
* element visibility (vt+0x70 0x1400db1c0, mode at params+0x10, `hud_bDrawHUD`-style global 0x142247288) and the HUD
  loop (0x1400d7be0 → 0x1400d6920) are the same for every element, so they pass if the wheel renders. `hud_bShowCrosshair`
  is 1 (not in `cvarShowDiffs` in `Sam4.log`, and `SafeMode.cfg` sets 1).

### A2. Second issue (after A1): no eye offset, so the crosshair has no correct depth (medium-high)
The lab run is **two-pass** (`vr_iUsingSinglePassStereo` = 0, `observe ... singlePass=0`): the view, HUD included, is
rendered once per eye with `device+0x18` = eye (EYE_TARGETS). In that mode:
* The HUD loop 0x1400d7be0 takes its VR branch (`device+0x08` = 1 on the immersive path, R10 §3) and places most
  elements on a lazily head-following 3D panel (`hep_qvVRPlacement` params+0x18, `hep_fVRStretch` +0x34, panel quat
  `hud+0x90`). The crosshair element ignores that: it calls 0x140b94200 (reset to a plain pixel ortho of the current
  drawport) at 0x140120db7 and draws in eye-image pixels. This is what we want, but it means the native depth has no
  effect. In two-pass the 2D flush 0x140c6c070 takes the mono branch (0x140b6b3a0 = 0 because `device+0x18` ≠ 0)
  and writes the same matrix row for both "eyes" at cb+0x40. The stereo depth at cb+0x58 therefore cannot create
  disparity, assuming the shader shifts by the row difference (inferred; the shader was not read).
* Position: the projection is already per eye (0x140478a80 → device slot 0x70 for `device+0x18`). The view is
  `player vt+0xdf8` (call **0x140120c42**, SM **0x140121660**, bytes `FF 90 F8 0D 00 00`, the only such call in the
  first 0x1000 bytes of render), which returns the **centre head** placement (P7: quat xyzw, then pos at +0x10).
  Both eyes therefore project the target from the same point. That puts the crosshair at the right direction but at
  infinite depth: about 1.8° of wrong vergence at 2 m, so near targets look doubled.

### A3. The mod's redirect logic is correct (high)
* Aim writer sites: `mov rcx, rsi` (the player) directly precedes both calls (0x1404a3a20, 0x1404a3e03). Polarity is
  right: `test eax,eax; jne` after 0x1404a3a23 selects the camera ray on non-zero, so 0 → `+0xf3c` = barrel. After
  0x1404a3e06, 0 → target = blocked origin with distance 0.
* Crosshair sites: `test eax,eax; jne 0x140121220` after 0x1401211e8 skips the projection on non-zero, and the ring
  recentres when non-zero. `hudGate` ignores RCX, which is right because RCX is not the player there.
* `findReturnOneCalls(crosshair_render, 0x1000)` covers 0x1401211e8 and 0x1401218d4 (the ring function starts at
  +0x8b0). Minor: `hudGate` also answers 0 when the HUD shows a spectated player, and `g_localVrd` is never
  cleared on level change (pointer compare only).

### A4. Fix plan
1. **Enable the predicate for VR players (required).** At install, inside `crosshair::install`, after the existing
   checks, locate the show predicate by a new signature `crosshair_show` (64-byte sig at Sam4 **0x1404947c0**, unique in
   Sam4, SamSM, Sam4_Unrestricted 0x140496ef0 and SamSM_Unrestricted 0x1404975d0; SM alt **0x140494eb0**; `port_check`
   mnemonic ratio 1.000). At function **+0xa0** (Sam4 0x140494860, SM 0x140494f50, both Unrestricted builds too) verify
   the 14 bytes `48 83 BB xx 25 00 00 00 0F 85 AF 00 00 00` with xx = `C8` (Sam4) or `D0` (SM). Then replace the 6-byte `jne` at
   +0xa8 (Sam4 **0x140494868**, SM **0x140494f58**) with `66 0F 1F 44 00 00`. Needs a small `patch::writeCode` helper
   (VirtualProtect + FlushInstructionCache); patch.hpp has none yet. Effect: only players with vrd change, and
   the predicate's only callers are the two crosshair gates. Install it only together with the F redirects (option
   `native_crosshair`). Without the F redirects the crosshair would show at the screen centre, which is wrong for hand aim.
   Do not patch the hidden-F stub, and do not null `player+0x25c8` around the call: other threads read vrd.
2. **Per-eye view (required for depth).** Redirect the view call at render 0x140120c42 / SM 0x140121660:
   overwrite `FF 90 F8 0D 00 00` with `E8 rel32` + `90` to a near thunk `crosshairView(player, P7* out, int flag)`.
   The thunk calls `(*player)[0xdf8/8](player, out, flag)`, then, if `device+0x18` ∈ {1,2} and the player is the local
   VR player, composes `out = out ∘ eyeInHead(eye)`: `out.pos += rotate(out.q, e.p)`, `out.q = out.q · e.q`, where
   `e` = the same pose the mod's slot 0x078 inverts (`toPose(v.inView[eye-1].pose)`, translation ÷ `worldScale()`).
   Use exactly that pose so the crosshair matches the world eye views. The projection is already per eye through slot
   0x070, so each eye projects the hit point `+0x2188` and the disparity comes out right. Leave
   `0x140c6c040(dp, max(D,0.5))` alone in two-pass (A2). If a runtime capture still shows an extra shift, force it to
   1.0 by redirecting the two depth-setter calls in render/ring (0x1401212a7 and the one in 0x140121410), as section 8 B
   says. The dispersion-ring distances use `vt+0x340` modes 0/1 and are unaffected.
3. **Single-pass (only if `vr_iUsingSinglePassStereo` is ever enabled).** One HUD pass with `device+0x18` = 0:
   0x140478a80 falls back to the player FOV, so hook it (SM 0x1404790b0) during the crosshair render so that it
   returns a centre-eye HMD projection, and keep the native depth, which the single-pass flush turns into
   disparity. Not needed for the current lab configuration.
4. **Behind-the-eye guard (polish).** 0x141152ae0 returns a visibility flag that the crosshair ignores. If the barrel
   points backwards, the point projects mirrored. Wrap crosshair vt+0x80 (vtable 0x1415ff2f0 / SM 0x1415e1c70, slot
   0x80) and skip the original call when `+0x2188` is behind the current eye. Do not write player state from that
   wrapper.
5. **Laser.** `prj_bRenderVRLaserSight` (0x142247fec, SM 0x142228c3c) is 1 by default and the mod never clears it. Set
   it to 0 through `se4/cvars` when `native_crosshair` is on (PLAN: no lasers; section 8 E assumed it was off).
6. **Diagnostics for the next lab run:** log once per level the show-predicate result for the local player, `device+0x18`
   during the crosshair render, `+0x2188`/`+0x2194`, and the projected pixel of each eye.

| item | Sam4 616154 | SamSM 628492 |
|---|---|---|
| show predicate (new sig `crosshair_show`) | 0x1404947c0 | 0x140494eb0 |
| vrd test / `jne` to NOP | 0x140494860 / **0x140494868** | 0x140494f50 / **0x140494f58** |
| predicate callers (gates) | 0x1401211b0, 0x140121aa0 | 0x140121b92, 0x1401224a0 |
| view call to redirect (`FF 90 F8 0D 00 00`) | **0x140120c42** | **0x140121660** |
| player projection (per eye in two-pass) | 0x140478a80 | 0x1404790b0 |
| 2D reset in render | 0x140120db7 → 0x140b94200 | 0x1401217d5 → 0x140b94860 |
| laser cvar | 0x142247fec | 0x142228c3c |

Confidence: A1 high, because the unconditional vrd test in the predicate shows statically that no crosshair can be
drawn for a VR player. A2 medium-high: the two-pass per-eye HUD and the per-eye projection come from code, the
2D shader's use of cb+0x58 is inferred. A3 high.

### A5. Lab after A1/A2 (2026-10-24, static follow-up)

Lab facts: the show predicate now returns 1 and render 0x140120b60 reaches the view thunk, but neither eye image
shows a crosshair or ring. No other HUD element shows either (health, armour, ammo). The only on-screen 2D is the
console notice "CHEATS ENABLED", printed through 0x1404270a0 → 0x140aa7a20 (console). The console draws inside
`Render`, not in the HUD. The weapon wheel shows only while the frame goes through the cinema path. New private
decompile: `lab/r8j.c` (CPlayerController vt+0x28 0x140415200, player views 0x14005be70, canvas flush 0x1400ea890,
`lab/r8j.txt`). `lab/r8_content/` holds a private extract of `Databases/HudParams.rsc` (from 70_616154_00_All_edit.gro),
which is gitignored.

#### Cause: in immersive VR the whole HUD, crosshair included, goes through an unauthored 3D "VR panel" path (high)
1. **The HUD render switches to 3D on `device+0x08`.** 0x1400d7be0 stores `0x140b6af60()` (= `device+0x08`, which is 1 on
   the immersive path, R10 §3) in `[rbp-0x64]`. The call is at **0x1400d7dc1**; SM 0x1400d7cc0 has it at **0x1400d7ea1**
   (→ 0x140b6b540). With the flag set:
   * the render builds a head-relative, lazily following panel (`hud+0x90`, thresholds 0x142247540..54c) and loads
     the **eye** matrices: two-pass uses slot 0x070 projection and slot 0x078 head→eye view, single-pass uses the union
     frustum and both eyes;
   * each element (0x1400d6920, SM 0x1400d6a00) clears the drawport's 2D flag (`mov [dp+0x30],0` at **0x1400d6a21**,
     SM 0x1400d6b01). It then sets the 2D object matrix (gfx+0xcd0.., +0xd90..) to
     `panel ∘ hep_qvVRPlacement (params+0x18 quat, +0x28 pos) ∘ scale(hep_fVRStretch params+0x34, y flipped)`.
     Element pixel coordinates therefore become head-space metres.
2. **SS4 never authored those placements.** Every element in `HudParams.rsc`, "Weapon crosshair", "Health", "Armor",
   "Current ammo" and the rest, has `hep_qvVRPlacement = { q 0,0,0,1; v 0,0,0 }` and `hep_fVRStretch = 1`. The panel origin is
   the head (the decompile of 0x1400d7be0 cancels the head position). So a quad at pixel (17, 440) is drawn
   17 m right and 440 m down, in the z = 0 plane through the eyes. That is outside the frustum and in front of the
   near plane, so nothing is visible. Fusion's VR games authored these values. SS4 shipped without VR.
3. **The crosshair cannot escape this by itself.** Render calls 0x140b94200 (pixel ortho) at 0x140120db7, but every
   quad goes through the HUD canvas 0x1400e8d20 → 0x1400e8d50. With `hud_bAllowBatching` (0x142247568, default 1)
   and an atlased texture, the quad is only **queued** (test at 0x1400e8e0f, SM 0x1400e8ee2). The queue entry is keyed by
   the current drawport and keeps the object matrix that was current when the entry was created (+0x1c..+0x48).
   The canvas flush **0x1400ea890** (SM 0x1400ea940), called at the end of the HUD render (0x1400d8e79), reads
   `0x140b6af60()` again (0x1400ea9b3, SM 0x1400eaa63). For each entry whose drawport has `+0x30 == 0` (test at
   0x1400eaaf5, SM 0x1400eaba5), which is the case after any VR element, it reloads the **eye** projection and head→eye
   view and draws with the stored object matrix. Only `+0x30 != 0` takes the pixel-ortho reset (0x140b94200). The
   crosshair's pixel coordinates are therefore also drawn as head-space metres, so they are invisible. This
   applies whether the pixel is at the centre or at the barrel projection.
4. **The cinema path works**, which is why the weapon wheel is visible. While the wheel is open the frame goes
   through the cinema path (`device+0x08` = 0, R11 §2.1). The HUD then takes its 2D branch, `[dp+0x30] = 1` with a pixel
   ortho, and the flush resets to 2D.

Other early-outs from question (1), checked and not the cause:
* camera mode: 0x1400d6920 skips `HCRM_NOT_RENDERED` elements (params+0x14 → element+0x20; checks at 0x1400d6987/0x1400d6991,
  SM 0x1400d6a68/0x1400d6a71) only when the HUD's view entity is not a puppet (class 0x1422817a0). The crosshair render is
  reached, so this check passes here.
* projection: 0x141152ae0's visibility flag is ignored. A sky shot still has a finite target (aim ray max distance), and
  with the eye offset the pixel lies in the image when the barrel points forward.
* texture: index = profile `+0x58` from 0x140478950 (default 4), clamped to `[0, count-2]`, with the null terminator dropped. It
  is never -1 or null for the shipped `cp_aptxCrosshairs`.
* colour: 0xffffffff by default. With `plr_iHealthBarOption` == 1 and a live puppet target it is the
  `cp_grCrosshairColorCoding` gradient. Neither colour makes it invisible.
* `prj_iGameOptionCrosshair` is read only by the options menu (0x140863481). The renderer uses the profile object
  above.

#### Fix
**Primary (cheapest, fixes the HUD and the crosshair at once):** in immersive VR, make the HUD render take its 2D
branch.
* Retarget the `E8` at **Sam4 0x1400d7dc1 / SM 0x1400d7ea1** (verify `E8 rel32` → 0x140b6af60 / 0x140b6b540) to a gate that
  returns 0 while the mod renders immersive stereo, and the original value otherwise.
* Leave the flush's own call alone. After the 2D branch the drawport has `+0x30 = 1`, so the flush takes 0x140b94200 (pixel
  ortho of the current eye drawport, 1344×1512 in two-pass).
* Result: the HUD loop runs once per eye pass. Every element is drawn screen-locked into each eye image, at
  infinity in two-pass. The crosshair is drawn at its per-eye projection (A2 eye view + slot-0x070 projection), so
  both eyes get the right disparity and depth.
* Optional comfort: pull elements in from the lens edges with `hud_fCustomHudScale` (0x142247550) or a safe-area
  margin. A later step can move the HUD to an XR layer (R11 §3).

**Alternative (true 3D HUD panel, more work):** keep the VR branch and write VR placements into each loaded
`CHudElementParams` at runtime: `+0x18` quat, `+0x28` pos, `+0x34` stretch, e.g. `v = (-W/2·s, H/2·s, -D)` with
`s = 2·D·tan(vfov/2)/H`. This writes in memory only, never to the file. The crosshair then still needs its own path,
because batching gives its quads an entry object matrix in head metres. Per eye, either disable batching around the
crosshair element or set the object matrix to `eyeInHead ∘ pixel→plane(depth D)` before its first draw. Use this
only if a head-locked 3D panel is wanted. It is not needed for a visible, depth-correct crosshair.

| item | Sam4 616154 | SamSM 628492 |
|---|---|---|
| HUD render | 0x1400d7be0 | 0x1400d7cc0 |
| **VR-flag call to redirect** (`E8` → device+0x08 getter) | **0x1400d7dc1** → 0x140b6af60 | **0x1400d7ea1** → 0x140b6b540 |
| per-element render / `mov [dp+0x30],0` | 0x1400d6920 / 0x1400d6a21 | 0x1400d6a00 / 0x1400d6b01 |
| canvas flush / its VR call / `+0x30` test | 0x1400ea890 / 0x1400ea9b3 / 0x1400eaaf5 | 0x1400ea940 / 0x1400eaa63 / 0x1400eaba5 |
| canvas draw / batching test (`hud_bAllowBatching`) | 0x1400e8d50 / 0x1400e8e0f (0x142247568) | – / 0x1400e8ee2 (0x142228458) |
| flush call in HUD render | 0x1400d8e79 | 0x1400d8f59 |

`port_check`: element render and flush match 1.000 with no field differences.

#### Cheapest diagnostics for the next run
1. In the new gate at 0x1400d7dc1: log once per 3 s `device+0x08`, `device+0x18`, and the drawport size
   (`*(gfx+0x1c78)` box +0x10..+0x1c). Expect 1 / 1-or-2 / 1344×1512 before the fix.
2. In the existing view thunk: log `*(int*)(*(gfx+0x1c78)+0x30)`. It is 0 when the 3D VR path is active (the cause)
   and 1 after the fix. Also log the projected pixel of `+0x2188` for that eye: rebuild it with 0x141152ae0 from the
   thunk's view and 0x140478b60.
3. One-line A/B without code: console `hud_bAllowBatching 0`. The crosshair should then appear, because the canvas draws
   immediately (0x1400e2560) with the crosshair's own pixel ortho. The rest of the HUD stays invisible, because its
   object matrices are still the 3D ones. If this happens, the deferred VR flush is confirmed as the cause.

Confidence: high for the cause. The VR branch, the zero placements in the shipped params and the flush's
`+0x30`/VR test are all read directly. The weapon wheel appearing only on the cinema path agrees with it. Medium for
the A/B test in diagnostic 3, because the immediate path's 2D vertex flush timing was not traced.

### A6. Lab after the HUD panel (2026-10-24, static follow-up)

Setup read from `src/se4/hud_vr.cpp`. A MinHook detour on the per-element render 0x1400d6920 temporarily writes
identity/(-W/2·s, H/2·s, 0)/s into `hep_qvVRPlacement`/`hep_fVRStretch` and replaces the anchor `*ctx[8]` with
`inverse(head)·panelWorld`. The crosshair is rendered with `*ctx[7] = 0` and `hud_bAllowBatching = 0`. Lab result: the
weapon wheel (render 0x1401298c0) is drawn correctly on the hand panel, while nothing else shows. New private decompiles:
`lab/r8k.c` (wheel, simple elements, canvas functions) and `lab/r8l.c` (the render functions from the lab log).

#### How the HUD canvas queue works (read from code, high)
* Every element has the HUD canvas `*(hud+0xb0)`, where `hud = ctx[10]`. Elements put quads and text into it through the
  texture draw 0x1400e8d20 → 0x1400e8d50 (SM 0x1400e8df0) and the text paths 0x1400e8290/0x1400e8340 →
  0x1400e87f0 (SM 0x1400e88c0).
* Textures are queued only while `hud_bAllowBatching` (0x142247568, SM 0x142228458) is set and the texture is
  atlased (test at 0x1400e8e0f, SM 0x1400e8ee2). **Text (0x1400e87f0) is always queued**: it does not read the cvar.
* A queue entry is found by **(current layer `canvas+0x10`, current drawport `gfx+0x1c78`)**. The 2D object matrix
  `gfx+0xcd0..0xd28` is copied into the entry (+0x1c..+0x48) **only when the entry is created** (0x1400e9010 for
  textures, 0x1400e8a43 for text). Later elements that queue into the same (layer, drawport) inherit that first
  matrix. Within the HUD render nothing changes the layer (the flush resets it to 0).
* Everything queued is drawn once, by the canvas flush **0x1400ea890** (SM 0x1400ea940) at the end of the HUD render
  (call 0x1400d8e79, SM 0x1400d8f59). For each entry:
  * if `entry.drawport+0x30` is 0 and `device+0x08` is set, the flush loads the eye projection (slot 0x070) and the
    head→eye view (slot 0x078), then the **stored** object matrix;
  * otherwise it calls 0x140b94200 (pixel ortho).

  The flush pushes and pops the gfx state (0x140b57340 / 0x140b585f0), frees all entries and sets `canvas+0x10 = 0`.
  **It can therefore be called again in the middle of the loop.**
* Immediate draws, used when batching is off and by the direct gfx text and rect calls, draw at once with the
  current matrices: 0x1400e2560 → 0x140b98570 → 2D cb 0x140c6c070 → gfx vt+0x228.

#### Why the wheel works and the others don't
1. **Simple elements cannot follow the panel (high).** Health, Armor, Lives, Netricsa notifier and the other
   `CSimpleHudElementParams` elements use render **0x1400cf310** → **0x1400ceb10** (SM 0x1400cf410 → 0x1400cec10).
   0x1400ceb10 saves `gfx+0xcd0..`, **overwrites** the object matrix with its own transform (rotation from the
   unnamed globals 0x142247514/518 = π, π, scaled by 1/1024, plus translation from the item's quat/pos) and draws
   immediately with gfx text (0x140b90e30/0x140b96160). Afterwards it restores the saved matrix. The panel matrix
   that 0x1400d6920 built is never used, so with the VR eye view these elements land in head space at "pixel"
   metres, which is invisible. Writing params cannot fix this.
2. **Shared queue entries (medium).** The wheel queues text through the canvas (0x1400e8290 ×2 in 0x1401298c0) and
   is drawn correctly by the flush, so the 3D flush path works. Other queued elements (canvas text in 0x1400cc7a0,
   0x1400f5390, 0x1400c8390; textures in 0x1400ee540, 0x1400c8390) are drawn with the matrix of whichever element
   created the (layer, drawport) entry first in that HUD pass. If that element is one the detour leaves alone,
   because its placement is not the default or the box test fails, or one that changes `gfx+0xcd0` before
   queueing, every later element inherits a wrong matrix. In wheel frames the wheel is effectively alone, since the
   crosshair and most elements return early while the wheel bit `+0x269c` is set, so its entry is right. Not
   proven: it needs the log below.
3. **Many "visible" elements draw nothing.** `he_bIsVisible` is only the element's state. For example 0x140052890 is
   `xor eax,eax; ret`, and several renders in the lab list contain no draw call. Judge success by the health, armour,
   ammo and crosshair elements only.
4. **Crosshair (open).** With batching off, its quads are drawn immediately in the pixel ortho its own 0x140b94200 sets
   up. If they are drawn, they land in the eye image. But the lab diagnostic runs at the view call 0x140120c42, which
   comes **before** these early-outs:
   * `en_ulPlayerPuppetFlags` bit 13 (`+0xfa8`, SM `+0xfb0`), test at 0x140120df8;
   * wheel bit `+0x269c` bit 0 (SM `+0x26a4`), 0x140120e07. The mod's wheel and menu-dummy machinery could leave it set;
   * no weapon `+0xf90` (SM `+0xf98`) and no ride `+0x75c`;
   * empty `cp_aptxCrosshairs`;
   * `+0x2568` (0x14049c010; SM `+0x2570`, 0x14049c600);
   * in the ring function, the show predicate.

   So "render reaches the view" does not show that anything was drawn.

#### Fix
1. **Flush per element.** In `hookedElement`, call the flush right after `realElement`, **before** restoring params,
   anchor and `dp+0x30`:
   `flush(*(void**)((uint8_t*)ctx[10] + 0xb0), gfx)` with flush = Sam4 **0x1400ea890**, SM **0x1400ea940**, and
   gfx = the gfx global (Sam4 0x1422aa9b0).
   * Each element's queued quads then use its own object matrix and its own 2D/3D flag.
   * The end-of-HUD flush finds an empty queue.
   * Do this for every element on the VR path, including the wheel and the crosshair. The crosshair flushes with
     `+0x30 = 1`, i.e. pixel ortho.
   * The cheaper `hud_bAllowBatching = 0` for the whole VR HUD is not enough, because text ignores it.
2. **Simple elements.** Leave their placement params at the default. Wrap 0x1400ceb10 (SM 0x1400cec10), or the element
   while its render is 0x1400cf310 (SM 0x1400cf410), so that the panel goes into the **view** instead of the
   object:
   * save `gfx+0xc70..+0xcc8` (3×4 doubles);
   * write `view = eyeView(slot 0x078) · inverse(head) · panelWorld · S`, where `S` = scale s, y flipped, origin
     moved to the drawport centre, the same mapping as the mod's placement;
   * clear the cached-matrix bits `gfx+0x16c0 &= 0xfffff3c0` (as 0x140b94200 does) and call `0x140b603d0(gfx, gfx+0xdf0, gfx+0xc70)`;
   * call the original, then restore the view.

   The element's own object matrix then maps its local space to pixels as on desktop, and the view carries the panel.
   Check the 1/1024 and π,π convention against the first capture: if the item comes out mirrored or tiny, fold the
   correction into `S`.
3. **Crosshair.** Keep `crosshair2d`. Add the per-element flush, which costs nothing because there are no queued
   textures. Fix whichever early-out the log below shows; most likely the wheel bit is left set by the wheel code.

| item | Sam4 616154 | SamSM 628492 |
|---|---|---|
| canvas flush (clears the queue, resets the layer) | 0x1400ea890 | 0x1400ea940 |
| end-of-HUD flush call | 0x1400d8e79 | 0x1400d8f59 |
| texture draw (wrapper → body) / batching test | 0x1400e8d20 → 0x1400e8d50 / 0x1400e8e0f | 0x1400e8df0 → ? / 0x1400e8ee2 |
| queued text (not gated by the cvar) | 0x1400e87f0 | 0x1400e88c0 |
| matrix capture at entry creation | 0x1400e9010 (tex), 0x1400e8a43 (text) | not ported |
| simple-element draw (overwrites the object matrix) / its element render | 0x1400ceb10 / 0x1400cf310 | 0x1400cec10 / 0x1400cf410 |
| 2D reset | 0x140b94200 | 0x140b94860 |
| crosshair early-outs | +0xfa8 b13, +0x269c b0, +0xf90/+0x75c, 0x14049c010 (+0x2568) | +0xfb0, +0x26a4, +0xf98/+0x75c, 0x14049c600 (+0x2570) |

SM ports come from unique 48-byte signatures (`sigscan`). The flush and element render also pass `port_check` at 1.000.

#### Cheapest logs
1. After `realElement`, per element (deduplicated like `logElement`), log the canvas layer count `*(int*)(canvas+0x20)`
   and, for layer 0, the entry count `*(int*)(*(canvas+0x18)+0x10)`. When the count grows, also log the new entry's
   translation column (+0x28, +0x38, +0x48 as floats). This shows which element creates the first entry and with
   which matrix.
2. In the crosshair view thunk, log the early-out fields `+0xfa8 & 0x2000`, `+0x269c & 1`, the `+0xf90` handle and
   `+0x2568`. Also log a counter incremented by a hook on the crosshair's texture draws: retarget the two `E8` to
   0x1400e8d20 inside 0x140121410, or count in a wrapper around 0x1400e2560 while the crosshair element runs.
3. Log whether 0x1400ceb10 runs (a counter) to confirm that health and armour take the simple path.

Confidence:
- High: the canvas-queue mechanics and the simple-element matrix overwrite (read from code).
- Medium: shared entries explain the canvas-drawn elements.
- Low: the crosshair cause (needs log 2).
