# R24: first-person riding view in VR (Sam4.exe 616154, SamSM.exe 628492)

Static analysis only. Sources:
- Ghidra decompiles from a private scratch copy of `lab/ghidra_r19` (`tools/ghidra/DecompileList.java`);
- capstone disassembly (`tools/disasm.py`), `callers.py`, `vcall_sites.py`, `vtable_of.py`, `cvar_readers.py`,
  `riprefs.py`, `field_refs.py`, `strrefs.py`, `meta_offset.py`, `meta_class.py`, `meta_enum.py`;
- a capstone scan of `.text` in Sam4 and SM for bit tests of seat-flag bits at `[reg+0x20]` (scratch script);
- masked-signature ports (`tools/port_list.py`, `sigscan.signature`/`find` against all four exes).

Nothing was run. No game file, cvar, user option, save or `src/` file was touched. Addresses are Sam4.exe VAs unless
marked **SM**.

Confidence tags (as in R17/R20/R21):
- **[D]**: read from a decompile or disassembly.
- **[M]**: from reflection or harvest.
- **[I]**: inference that needs a runtime check.

## 0. Answers

1. **The single decision point is the puppet virtual `vt+0xdf0`, "is in third-person view", `0x1402be4d0`**
   (**SM 0x1402be2c0**). **[D]**
   - Signature: `BOOL __fastcall is_third_person_view(CPuppetEntity* this)`. It has one argument (RCX), returns 0/1 in
     EAX and has no side effects.
   - It is the only implementation of slot 0xdf0 in 51 puppet vtables, including the player's (Sam4 0x1418374b0,
     SM 0x14181aea0). It has no direct callers and 48 `call [reg+0xdf0]` sites.
   - Its callers include:
     - the player render view 0x14048dad0 (first-person or third-person body handling);
     - the view placement `vt+0xdf8` 0x1402a42a0 (head/eye camera or third-person camera);
     - the vehicle operated step;
     - the rider motion handler;
     - FOV and the weapon barrel.
   - While riding, it decides in this order (section 2):
     1. The seat allows only third person (`(psd_sfSeatFlags & 0xC000) == SF_ALLOW_THIRD_PERSON_VIEW`): it returns 1.
     2. The seat allows only first person (`== SF_ALLOW_FIRST_PERSON_VIEW`): it returns 0.
     3. Otherwise it returns **`en_bAvatarViewMode` (+0x788)**.
2. **`SF_FORCE_3RD_PERSON_VIEW` (0x100) is not read at runtime.** **[D]** for the converter, **[I]** for "no other
   reader" (based on a scan).
   - Its only reader in either build is the `CPuppetSeatData` load-upgrade callback **0x140281840**
     (SM 0x140285500). For data older than version 0x25 it rewrites the flags:
     - FORCE_3RD becomes "third person only" (clears 0x4000, sets 0x8000);
     - otherwise FORCE_1ST (0x400) becomes "first person only";
     - otherwise both are allowed.
   - What matters at runtime is the pair of ALLOW bits (section 3).
   - The SS4 ATV/Chopper/Tractor and SM Snowmobile_v2 seats have both ALLOW bits in the file (0xc1xx). So the view is
     `en_bAvatarViewMode`, unless the upgrade ran and made them third-person only. That is a runtime check (section 6).
     The hook below covers both cases.
3. **Where `en_bAvatarViewMode` comes from (the riding-view option chain).** **[D]**
   - `get_camera_mode` **0x1404789f0** (SM 0x140479020) returns a `PlayerCameraMode`
     (`PCM_FIRST_PERSON` 0, `PCM_LEFT_SHOULDER` 1, `PCM_BEHIND_PLAYER` 2, `PCM_RIGHT_SHOULDER` 3) and stores it in
     `en_pcmPlayerCameraMode` (+0x24e0, SM +0x24e8). Its source depends on whether the player is riding
     (`is_riding` 0x1402ad380):
     - riding: the local profile's `po_iGameOptionPreferredRidingView` (`CProfileOptions` +0x14; +0x44 from the
       pointer that `0x140478950` returns), or the cvar `prj_iGameOptionPreferredRidingView` when there is no profile;
     - not riding: `po_iGameOptionPreferredView` (+0x40), or the cvar `prj_iGameOptionPreferredView`.
   - **`update_avatar_view` = player `vt+0x1098` 0x140424730** (SM 0x1404247c0) runs on the local player. It sets
     `en_bAvatarViewMode = (camera mode != 0)` through `set_avatar_view` 0x1404303f0 (SM 0x140430470), which also
     sends it to the server.
   - The CPSRiding state calls it after boarding (0x1405d99e0) and when leaving the seat (0x1405d9c00).
   - **On boarding, the native seat step writes the profile's riding view itself.** The CPSRiding seat-rider step
     0x1405db250 (called from Enter, vt+0x90) calls `set_riding_view(player, psd_psvPreferredSeatView ? 2 : 0)`
     (0x140494530, SM 0x140494c20). That function writes `po_iGameOptionPreferredRidingView` and
     `en_pcmPlayerCameraModeRiding` (+0x24ec), but only when the seat allows that view.
   - **So a seat whose preferred view is `PSV_THIRD_PERSON` puts every rider into the chase camera.** This is the
     likely cause of the lab observation on the Snowmobile **[I]**: its PSV value was not decoded.
   - `plcmdThirdPersonView` toggles the view mid-ride: 0x140432b80 (`vt+0x1090`) and 0x140404750.
   - Answer to the R7 open item: the first-person value of `prj_iGameOptionPreferredRidingView` is
     **`PCM_FIRST_PERSON` = 0**.
   - Second R7 open item: a third-person ride view does bypass vrd. `vt+0xdf8` then calls the third-person camera
     placement 0x1402a9070 and never `vt+0x340`, so the device head pose is not used.
4. **Recommended intervention: a MinHook detour of `is_third_person_view`.**
   - It returns **0 for the local VR player while riding** and calls the original for everything else (section 5).
   - Nothing is written: no cvar, profile option, member or net state.
   - The render view, the camera, and the rider and vehicle code on the local machine all see first person.
   - `vt+0xdf8` then returns `vt+0x340` mode 0, which **with vrd is the head world placement** (0x14043cb50,
     SM 0x14043cc90). That placement already follows the seat (R21 §3, R7 §4).
   - Both builds match one 64-byte masked signature, unique in all four exes (section 7).
   - A second detour on `get_camera_mode` is an optional companion (section 5.3). It is not needed for the view.

Confidence:
- **High** that `vt+0xdf0` is the gate between the head camera and the chase camera. Both callers on the view path
  were read, and 0x1402a42a0 / 0x1404794a0 were read.
- **High** for the option chain and the enum values (meta enum + decompile).
- **High** for the SM port: the function is byte-identical except for rel32 targets; signature, vtable slot and member
  offsets were checked.
- **Medium** for the side effects listed in section 4: they were read from the consumers, but nothing was observed.

## 1. Seat and camera enums [M]

| enum (Sam4 table / SM table) | values |
|---|---|
| `SeatFlags` 0x1416d5ad8 / SM 0x1416b8848 | 0x1 RIDER_CAN_SHOOT_OWN_WEAPON, 0x2 WAY_POINT_SEAT, 0x4 PUPPETS_CANT_SEAT, 0x8 RIDER_DIES_WITH_RIDE, 0x10 PLAYER_CANT_SEAT, 0x20 PLAYER_CAN_BE_DAMAGED, 0x40 NON_PLAYER_IS_INVOULNERABLE, 0x80 PLAYER_CANT_EXIT, **0x100 FORCE_3RD_PERSON_VIEW**, 0x200 FIXED_FP_VIEW, 0x400 FORCE_1ST_PERSON_VIEW, 0x800 COPY_ALIGNMENT, 0x1000 ALLOW_DUAL_WIELDING, 0x2000 USE_3RD_PERSON_OPERATOR_INPUT, **0x4000 ALLOW_FIRST_PERSON_VIEW**, **0x8000 ALLOW_THIRD_PERSON_VIEW**, 0x10000 DISABLE_RIDER_STRAIGHTENING, 0x20000 ALWAYS_SHOW_CROSSHAIR, 0x80000 FORCE_OPERATOR_AS_DAMAGE_INFLICTOR, 0x100000 USE_RIDER_HEADING |
| `PreferredSeatView` 0x1416d5d10 / SM 0x1416b8a80 | 0 PSV_FIRST_PERSON, 1 PSV_THIRD_PERSON |
| player camera mode 0x14182c090 / SM 0x14180f990 | 0 PCM_FIRST_PERSON, 1 PCM_LEFT_SHOULDER, 2 PCM_BEHIND_PLAYER, 3 PCM_RIGHT_SHOULDER, 4 PCM_COUNT |
| `PuppetNetFlags` (`en_pnfNetFlags` +0x7a4) 0x1416cfce8 | used here: 0x1000 AVATAR_SEEN_IN_1ST_PERSON, 0x10000 ALIVE, 0x80000 1ST_PERSON_AVATAR_WITH_WEAPON, 0x1000000 1ST_PERSON_AVATAR_INTERRUPTIBLE, 0x2000000 GRIPPED_BY_SANDWHALE |

`CPuppetSeatData` members (Sam4 = SM):

| member | offset |
|---|---|
| `psd_sfSeatFlags` | +0x20 |
| `psd_psvPreferredSeatView` | +0x24 |
| `psd_vFirstPersonViewOriginStaticOffset` | +0x70 |
| `psd_vFirstPersonViewOriginDynamicOffset` | +0x7c |
| `psd_idFirstPersonViewMoverBone` | +0x88 |
| `psd_fMinLookPitchDeg` / `MaxLookPitchDeg` / `MaxHeadTurnDeg` | +0x90 / +0x94 / +0x98 |
| `psd_fMaxUseDistance` | +0xac |

`psd_fMaxUseDistance` at +0xac resolves R21's "`seat+0xac` taken to be a use radius".

Player members:

| member | Sam4 | SM |
|---|---|---|
| `CPuppetEntity::en_bAvatarViewMode` | +0x788 (change time +0x78c) | same |
| `en_b3rdPersonCameraDisabled` | +0x42c | same |
| `en_idCurrentSeat` | +0x2c0 | same |
| `en_henRide` | +0x75c | same |
| `en_hcc3rdPersonRideCamera` | +0x3a8 | not checked |
| `CPlayerActorPuppetEntity::en_pcmPlayerCameraMode` | +0x24e0 | +0x24e8 |
| `en_pcmPlayerCameraModeRiding` | +0x24ec | +0x24f4 |
| `en_qRelLookToVehicleSeat` | +0x24c0 | not checked |
| vrd | +0x25c8 | +0x25d0 |

`CProfileOptions` (Sam4 = SM):

| member | offset |
|---|---|
| `po_iGameOptionPreferredView` | +0x10 |
| `po_iGameOptionPreferredRidingView` | +0x14 |
| `po_iGameOptionThirdPersonCamera` | +0x18 |

All of these are **persistent profile options**. The mod must not write them.

## 2. `is_third_person_view` 0x1402be4d0 (SM 0x1402be2c0) [D]

```text
BOOL is_third_person_view(CPuppetEntity* p):                 vt+0xdf0
  if IsTalos(p->world) && (p.netflags & 0x1081000): return 0  (Talos only; IsTalos is false in SS4/SM, R3)
  if p.netflags & GRIPPED_BY_SANDWHALE:              return 1
  ride = handle(p.en_henRide)
  if ride:
     if ride.vt[0x868](p.en_idCurrentSeat): return 1    0x1402c04f0: (seat.flags & 0xC000) == 0x8000 (TP only)
     if ride.vt[0x870](p.en_idCurrentSeat): return 0    0x1402c0530: (seat.flags & 0xC000) == 0x4000 (FP only)
     if p.vt[0xec8]() && !(ride.netflags & ALIVE): return 1   riding a dead CMechPuppetEntity [I: class 0x14228d190]
  if !(p.netflags & ALIVE):          death camera rules (state +0x860, +0x410 & 0x800, +0xa28, death-camera option)
  if !p.vt[0xc20]():                 return 0             0x14047ed40: the right-hand weapon refuses third person
  if p+0x9e8 != -1:                  return p+0x9e8 == 1  (scripted override)
  if p.en_b3rdPersonCameraDisabled:  return 0
  return p.en_bAvatarViewMode
```

The SM function matches instruction for instruction; only the rel32 targets differ (IsTalos SM 0x1400a8ee0,
handle→entity SM 0x141113500).

**View path that uses it** **[D]**:

- **render view 0x14048dad0** (SM 0x14048e0e0):
  - `third = vt+0xdf0(player)`.
  - If `third == 0`: the own body is hidden from the own view (0x140ca3280(player)). With
    `PNF_AVATAR_SEEN_IN_1ST_PERSON` the avatar is drawn instead with the `HideBody`/`HideArms` masks.
  - If `third == 1`: the body is drawn normally.
  - Then `camera = vt+0xdf8(player)`.
- **view placement `vt+0xdf8` 0x1402a42a0** (SM 0x1402a4060):
  - `vt+0xdf0` true → `0x1402a9070` (SM 0x1402a8e60), the third-person camera. While riding, it recurses into the
    ride's own camera placement.
  - false → `vt+0x340` 0x1404794a0 (SM 0x140479b00), mode 0. If vrd (+0x25c8 / SM +0x25d0) is non-null it returns
    the **vrd head world placement** 0x14043cb50 (SM 0x14043cc90). Otherwise it returns the body view +0xf20.

## 3. FORCE_3RD_PERSON_VIEW and the seat ALLOW bits [D]

**`CPuppetSeatData` upgrade callback 0x140281840** (SM 0x140285500):
- It is registered next to the class meta (`.rdata` 0x1416d6720, flag 0x80, class global 0x1422816e0).
- If `*(class+0x48)->+0x18 < 0x25` (taken to be the loaded data version **[I]**), it rewrites the flags:
  - `if flags & 0x100: flags = flags & ~0x4000 | 0x8000` (third person only);
  - `elif flags & 0x400: flags = flags & ~0x8000 | 0x4000` (first person only);
  - `else flags |= 0xC000` (both allowed).

**Other readers** (scan of `.text` of both exes for `test/and [reg+0x20],imm` with bit 8, and for `bt reg,8` within 6
instructions of a `[reg+0x20]` load):
- In the game-code range (0x140200000–0x140800000) the only hit is the upgrade callback.
- The remaining hits are in engine code (0x1401xxxxx / 0x141xxxxxx) on unrelated structures.
- The scan is a heuristic **[I]**.

**ALLOW bit readers, all [D]:**
- `ride vt+0x868` 0x1402c04f0 (SM 0x1402c02e0) and `vt+0x870` 0x1402c0530 (SM 0x1402c0320): used by the predicate.
- `set_riding_view` 0x140494530 (SM 0x140494c20): it accepts mode 0 only with 0x4000 and mode 2 only with 0x8000.
- The boarding hint in player `vt+0x830` 0x1404864c0 (SM 0x140486a40): it shows "Toggle view"
  (`{plcmdThirdPersonView}`) when `prj_hmGameOptionShowHints == 2` and both bits are set.

**Seat flags from R21 §1.4** (file values, before any upgrade):

| seat | flags | ALLOW bits | effect |
|---|---|---|---|
| ATV | 0xc125 | both | the riding view follows the option/PSV |
| Tractor | 0xc105 | both | the riding view follows the option/PSV |
| Chopper | 0xc12d | both | the riding view follows the option/PSV |
| Harvester | 0xc02d | both | the riding view follows the option/PSV |
| SM Snowmobile_v2 | 0xc125 | both | the riding view follows the option/PSV |
| SM Tractor / TransportCart | 0xc105 | both | the riding view follows the option/PSV |
| Mech | 0x468c | first person only | always first person |
| SM Babymech | 0x4684 | first person only | always first person |

If these files are older than version 0x25, the upgrade turns every FORCE_3RD seat into third-person only, and
`ride vt+0x868` then forces the chase camera whatever the option says. Either way, **only the predicate hook covers both
cases**; a `get_camera_mode` hook alone would not cover the upgraded case.

## 4. What changes when the predicate answers "first person" for the local rider

Consumers of `vt+0xdf0` that matter while riding:

| consumer | Sam4 | SM | with the hook (local VR rider) | conf. |
|---|---|---|---|---|
| render view | 0x14048dad0 | 0x14048e0e0 | first-person branch, the same as on foot: own body hidden from the own view, or masked avatar with PNF_AVATAR_SEEN_IN_1ST_PERSON. Whatever `src/se4/body.cpp` does on foot now applies while riding. The vehicle is a separate entity and is still drawn. | D |
| view placement `vt+0xdf8` | 0x1402a42a0 | 0x1402a4060 | `vt+0x340` mode 0 = vrd head placement (anchor ∘ device head). The anchor follows the seated player (R21 §3, R7 §4), so the view sits at the seat. `psd_vFirstPersonViewOrigin*` / `psd_idFirstPersonViewMoverBone` are **not used** on this path: mode 0 returns the vrd head before it reads +0xf20. Their reader was not traced; it is presumably the flat body-view rebuild 0x1404795a0. | D / I |
| rider motion handler (CVehicleHandlingMotionHandler) | per-step 0x14023cc70; init 0x14023a8a0; rider step 0x14023d990 | rider step 0x14023d5b0 (R7) | Per-step: first-person branch 0x14023d5c0 instead of third-person 0x14023d730; stores the result at handler+0x174. Init: skips the third-person setup 0x14023b1f0. Rider step: 0x140943e60 instead of 0x140943e40. These are the native **first-person riding animations** (the first-person arms pose on the bars). How they look with the VR hands/body is a lab check. Remote machines are not affected by the local hook. Their predicate reads whatever `en_bAvatarViewMode` they hold for this player. `set_avatar_view` sends it to the server; whether the server forwards it to other clients was not traced **[I]**. | D / I |
| vehicle operated step `vt+0xb88` | 0x1402baa50 | 0x1402ba8a0 | The third-person operator-input branch (aim from the chase camera's `camera_aim_ray`) runs only if third person **and** one of: bot view; seat `SF_USE_3RD_PERSON_OPERATOR_INPUT` (0x2000); ride-of-ride with `vt+0x668 == 0`. None of the listed seats has 0x2000, and `vt+0x668` = the global 0x142247f70 (1 except during auto-test bots). So **driving input is unchanged**. | D |
| FOV 0x140477aa0 | 0x140477aa0 | not ported | First-person riding uses the ride params' +0x50c FOV override and `plr_fFOVRideOverride`. Irrelevant in VR (the device projection wins). | D / I |
| weapon barrel 0x1402d7220 | 0x1402d7220 | not ported | No change for VR weapons: with +0x1f0 the model barrel is used anyway (R3). | D |

**Not changed by the hook:**
- `en_bAvatarViewMode`, `en_pcmPlayerCameraMode(Riding)` and the profile options keep their native values. If the
  boarding seat's PSV is third person, `en_bAvatarViewMode` is 1 and has been sent to the server.
- Remote machines therefore keep the native view state of the VR player (a third-person rider when the PSV says so).
  They draw the body in third person anyway, so nothing visible changes for them.
- The native code still writes `po_iGameOptionPreferredRidingView` on boarding (0x1405db250 → 0x140494530) and on
  `plcmdThirdPersonView`. These are the native writes that happen in flat play too. **The mod adds no write.**
- The boarding hint "Toggle view" may still appear. Pressing the toggle no longer changes what the VR player sees.

**Not covered:** the native first-person riding setup inside `update_avatar_view`. It runs only when the camera mode
is 0: it resets `en_qRelLookToVehicleSeat` (+0x24c0) and sets the look heading from the seat placement (`vt+0x7b0`).
With the predicate hook alone it does not run when the option/PSV says third person. In VR the head is not taken from
the look, so this is expected to be harmless **[I]**. If the lab shows a look/aim heading offset while riding, add the
companion detour in section 5.3.

## 5. Implementation recipe (`src/se4/vehicles.cpp`)

### 5.1 Symbols
- This gate added `is_third_person_view` and `get_camera_mode` to `signatures.json`, ran `tools/port_symbols.py`
  (`manual.json` now holds all four builds; the run's FAIL lines were already there before this gate), and appended
  `manual:is_third_person_view` / `manual:get_camera_mode` to `tools/symbols.txt`.
- `src/se4/symbols_gen.hpp` was **not** regenerated, because this gate does not touch `src/`. Run
  `tools/gen_symbols.py` as the first implementation step.
- The other needed pieces already exist in the module: `handle_entity`, member `CPuppetEntity_en_henRide`,
  `player_vr::localVrd()` and `player_vr::vrdOffset()`.

### 5.2 Detour (install from `vehicles::install()`; independent of `vehicle_grab`)

```cpp
using ThirdPersonFn = int(__cdecl*)(uint8_t* puppet);   // BOOL is_third_person_view(CPuppetEntity*), vt+0xdf0
ThirdPersonFn realThirdPerson;

// Called for every puppet from the simulation and render paths (48 sites): keep it to pointer compares.
int __cdecl hookedThirdPerson(uint8_t* puppet) {
    void* vrd = player_vr::localVrd();
    if (vrd && puppet && *static_cast<uint8_t**>(vrd) == puppet          // vrd+0 = its player (vrd_init)
        && at<void*>(puppet, player_vr::vrdOffset()) == vrd              // safe to read: puppet is that player
        && vrdev::installed()
        && handleEntity(at<int32_t>(puppet, se4::build().off(se4::gen::CPuppetEntity_en_henRide)))) {
        static LONG once;
        if (!InterlockedExchange(&once, 1)) VRLOG("vehicles: riding view kept first person for the VR player");
        return 0;                                                         // PSV/PCM first person: head camera
    }
    return realThirdPerson(puppet);
}
// install:
if (void* fn = b.at<void>(se4::gen::is_third_person_view); fn && se4::options::get("ride_first_person", 1))
    MH_CreateHook(fn, reinterpret_cast<void*>(hookedThirdPerson), reinterpret_cast<void**>(&realThirdPerson)),
    MH_EnableHook(fn);
```

Notes:
- **Local VR player test.** The detour compares `puppet` with `vrd[0]` before it reads `puppet+vrdOffset`. Reading
  +0x25c8 / +0x25d0 of an arbitrary (smaller) puppet would go out of bounds, so this order makes the read safe.
  `localVrd()` is never cleared today. If a level change can leave it dangling, have `player_vr` clear it when the
  player goes away. That is a separate small fix and is not required by this hook.
- **Riding test.** `en_henRide` resolving to an entity is the same test the predicate uses. Dropping it would also
  keep the VR player out of the death camera, the sandwhale grip and the `plcmdThirdPersonView` toggle on foot. That
  is plausible for VR, but untested; keep it riding-only first.
- **MinHook safety.** The prologue `48 89 5C 24 08 57 48 83 EC 20` relocates cleanly, and no branch in the function
  targets its first 5 bytes. The detour is thread-safe: it only reads and does not lock.
- **Diagnostic** (with `vehicles_log`). Once per boarding, log `seat = ride vt+0x780(en_idCurrentSeat)`,
  `*(u32*)(seat+0x20)`, `*(i32*)(seat+0x24)`, `en_bAvatarViewMode` (+0x788) and `en_pcmPlayerCameraMode`
  (+0x24e0 / SM +0x24e8). This settles section 6 items 1–2.

### 5.3 Optional companion (only if section 4's "not covered" item shows up in the lab)

- Detour `get_camera_mode` 0x1404789f0 (SM 0x140479020): `int __fastcall(CPlayerActorPuppetEntity*)`.
  - For the local VR rider, call the original, then write `player+0x24e0` (SM +0x24e8) = 0 and return 0.
  - The +0x24e0 store is a plain in-memory member, not an option or cvar.
- `update_avatar_view` then sets `en_bAvatarViewMode = 0` on boarding and also runs the native first-person seat-look
  setup. As a side effect, that state is sent to the server (`set_avatar_view`), the same as for a flat player who
  rides in first person.
- It does **not** cover upgraded third-person-only seats, so it is not a replacement for 5.2.
- Do **not** hook `set_riding_view` (0x140494530) or write any `po_`/`prj_` riding option: those are saved.

## 6. Open items (runtime checks)

1. Runtime seat flags of the SS4 ATV and SM Snowmobile_v2: are they still 0xc1xx, or 0x81xx after the 0x25 upgrade?
   Use the diagnostic in 5.2.
2. `psd_psvPreferredSeatView` of those seats. Expected PSV_THIRD_PERSON, given the observation; not decoded from the
   `.ep`.
3. What the first-person rider-handler variants (0x14023d5c0, 0x140943e60) do to the local body and arms with the
   mod's VR hands/body, e.g. whether the first-person arms snap to the bars.
4. Whether the look heading needs the native first-person seat-look reset (section 5.3).
5. Class global 0x14228d190 = `CMechPuppetEntity`. It is inferred from the 0x1058-byte allocation at 0x140747253,
   whose ctor stores a vtable next to the `CMechPuppetEntity` meta strings.

## 7. Address table

| item | Sam4 616154 | SamSM 628492 | method / conf. |
|---|---|---|---|
| **is_third_person_view** (puppet vt+0xdf0) | **0x1402be4d0** | **0x1402be2c0** | sig 64 B unique in all 4 (Sam4_Unrestricted 0x1402c0b90, SamSM_Unrestricted 0x1402c0970); SM player vtable 0x14181aea0 +0xdf0. D |
| get_camera_mode | 0x1404789f0 | 0x140479020 | sig 64 B unique in all 4 (Sam4_U 0x14047b120, SamSM_U 0x14047b740); SM reads +0x24e8. D |
| local profile (→ CProfileOptions at +0x30) | 0x140478950 | 0x140478f80 | port_list 24 B; SM call in get_camera_mode. D |
| is_riding | 0x1402ad380 | 0x1402ad1a0 | port_list. D |
| update_avatar_view (player vt+0x1098) | 0x140424730 | 0x1404247c0 | SM vtable slot. D |
| set_avatar_view (+RPC) | 0x1404303f0 | 0x140430470 | SM call in update_avatar_view. D |
| toggle view (player vt+0x1090) / plcmd handler | 0x140432b80 / 0x140404750 | 0x140432c50 / 0x140404630 | SM vtable slot / port_list. D |
| set_riding_view / set_view | 0x140494530 / 0x1404945d0 | 0x140494c20 / 0x140494cc0 | bit scan + SM callers. D |
| CPSRiding seat-rider step (call to set_riding_view) | 0x1405db250 (call 0x1405db602) | 0x1405db080 (call 0x1405db432) | port_list + callers. D |
| CPSRiding after-enter / leave (call vt+0x1098) | 0x1405d99e0 / 0x1405d9c00 | 0x1405d9810 / 0x1405d9a30 | port_list. D |
| seat TP-only / FP-only (ride vt+0x868 / 0x870) | 0x1402c04f0 / 0x1402c0530 | 0x1402c02e0 / 0x1402c0320 | port_list 48 B. D |
| CPuppetSeatData upgrade (FORCE_3RD reader) | 0x140281840 | 0x140285500 | port_list + bit scan. D |
| render view / view placement vt+0xdf8 | 0x14048dad0 / 0x1402a42a0 | 0x14048e0e0 / 0x1402a4060 | port_list; SM vtable. D |
| view by mode vt+0x340 / third-person camera placement | 0x1404794a0 / 0x1402a9070 | 0x140479b00 / 0x1402a8e60 | SM vtable / SM call in vt+0xdf8. D |
| player on-ride vt+0x830 (hint) | 0x1404864c0 | 0x140486a40 | SM vtable. D |
| IsTalos | 0x1400a8cc0 | 0x1400a8ee0 | R3; SM call in predicate. D |

`signatures.json` entries added by this gate (`tools/port_symbols.py` was run, so `manual.json` has all four builds;
`symbols_gen.hpp` is not regenerated yet):

```json
"is_third_person_view": {"src": "0x1402be4d0", "len": 64, "_note": "BOOL (CPuppetEntity*): puppet vt+0xdf0; seat ALLOW bits, else en_bAvatarViewMode. False = vt+0xdf8 uses vt+0x340 mode 0 (vrd head) (R24)"},
"get_camera_mode": {"src": "0x1404789f0", "len": 64, "_note": "int (CPlayerActorPuppetEntity*): PCM_* from the profile/prj (riding) view option, stored in en_pcmPlayerCameraMode (R24)"}
```
