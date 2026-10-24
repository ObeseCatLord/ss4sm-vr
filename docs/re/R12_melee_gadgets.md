# Gate R12: physical melee and gadgets in the hand (Sam4.exe 616154, SamSM.exe 628492)

Static analysis only (Ghidra copy `lab/ghidra_r12`, decompiles in `lab/r12/dec/*.c`; nothing was run). Addresses are
Sam4.exe PE VAs unless marked SM. "vrd" is `CPlayerVRData` (player+0x25c8, SM +0x25d0, R3). "player" is
`CPlayerActorPuppetEntity`. `vt+0xNN` is a vtable slot; a vtable start is the one a constructor stores (the
`vtable_slot.py` rule). Member names and offsets come from the reflection records (`meta_class.py`,
`meta_members.py`); CName globals were named with `cname_globals.py`. Content facts come from class and file
names in the CTSEMETA files (`strings`, extracted to `lab/r12/ss4`, `lab/r12/sm`). Member *values* inside .ep/.rsc
files were not decoded (no binary-meta value reader exists yet), so per-asset numbers are unknown.

New read-only tools:
* `tools/class_vtables.py <exe> <fn> [slot]`: every constructor-stored vtable that holds a function, labelled
  with the nearest preceding class-name string (SE4 puts a class's name, its meta records and its vtables in one
  .rdata block). The label can be off by one class; confirm it with member offsets.
* `tools/vtable_diff.py <exe> <base-vt> <derived-vt>`: the slots a derived class overrides.

## 0. Answers in short

* **A1.** SS4/SM player melee is (a) the knife, `CMacheteWeaponEntity` (`KnifeWeapon.ep`, SM `KnifeWeapon_SM.ep` +
  pickup `Knife_SM.ep`): a canned arc of thick rays from the **body**, damage `DMT_CUT`; (b) picked-up melee items,
  `CGenericMeleeWeaponEntity` (`GenericMeleeWeapon.ep`, traffic signs and the like), same arc; (c) **melee kills**
  (the "brutal" finishers) on `plcmdUse` ("Use/Melee"): a Lua-chosen synchronized animation, not a hit test. The
  sledgehammer, chainsaw, axe and baseball bat classes exist (Fusion/SS3 leftovers), but no SS4/SM player content
  uses them. Damage is `target->vt[0x400](target, ctx, const CDamage*)`, and health changes **only on the
  authority** (0x1402b57a0).
* **A2.** The `vr_fMinSwingToHitSpeed` reader 0x14079ecd0 belongs to **`CObjectHolderWeaponEntity`** (vtable
  **0x141abcd20**, slot **+0x210**; R3's "vt 0x141abc9a8 +0x588" was a backward-walk artefact). It breaks barriers
  with a carried object when the contact-detector speed `en_fContactDetectorVelocity` (+0x3dc, present on **every**
  `CBaseWeaponEntity`) exceeds the cvar. It is not a melee weapon path, but it is a complete native template:
  contact speed, then native damage, then haptic pulse.
* **A3.** No native client-to-server "apply damage" call exists. `DoUseMeleeAttack` (0x140473b40) is a net call, but
  its server side starts a canned kill. Recommended: run the blade sweep **on the authority**, from the hand poses
  the server already receives (`ClientActionVR`, R3 s4), using the native segment cast 0x1401a4f70 with a radius
  (result +0x98), and apply native damage exactly the way the knife does (0x14078f3b0). Section 15.
* **B4.** A gadget in use is the **`CGadgetHolderWeaponEntity`** weapon (`GadgetHolder.ep`, vtable 0x141a9d040).
  Its model is the gadget's first-person `*_Arms.mdl` (`gip_pmcFirstPersonModel`). It is placed by the normal
  weapon placement 0x1402d8ad0, so **with vrd it already follows the VR hand**. Throwables are an AOE targeting mode
  plus a launch mode; the launch (0x140a6f5a0, authority only) passes an explicit **direction**
  (`bpp_vLaunchDirectionOverride`) and **speed** (`bpp_fLaunchVelocity`). **R6: yes**, so hand velocity can replace
  the aimed velocity.
* **B5.** Use the native command path (OnHudCommand 0xc / holder `vt+0x8a0`). Overwrite the launch mode's stored
  direction and speed with the release velocity, estimated on the authority from the replicated hand poses. Activated
  gadgets fire on squeeze through the same command. Section 15.

## 1. Melee content and commands

| item | evidence |
|---|---|
| `plcmdUse` = "Use/Melee" (no separate melee command) | `CommandTable.lua` line 24; VR binding `AddDefaultBinding(1,"VRController","ButtonB","plcmdUse")` (`DefaultVRInputControllerBindings.lua`) |
| Knife = `CMacheteWeaponParams` | `KnifeWeapon.ep`, `KnifeWeapon_DM.ep` (SS4), `KnifeWeapon_SM.ep` (SM). Models `Models/Weapons/Machete/Knife{,_Arms,_Item}.mdl`. Only SS4 database that refers to it: `HudParams.rsc` (wheel); SM also has the pickups `Items/Weapons/Knife.ep`, `Knife_SM.ep`. How SS4 hands it out (level .wld) was not scanned: unknown |
| Generic melee items = `CGenericMeleeWeaponParams` | `GenericMeleeWeapon.ep`; thrown as `CGenericItemProjectileParams` (`Projectiles/GenericMeleeWeapon/Traffic_Sign_*.ep`). Global params `ggp_pgmwpGenericMeleeWeapon` +0xc68 |
| Fists/hands = `CHandsWeaponParams` | `HandsWeapon.ep` (`DefaultWeapons.rsc`; `ggp_phwpHandsWeapon` +0xd58). Hands carry the Sirian bracelet, scaffold pole and torn-off gibs (`hwp_*` members) |
| ObjectHolder = `CObjectHolderWeaponParams` | `ObjectHolder.ep` (`ggp_pwpObjectHolder` +0xc58) |
| Melee kill selection | `Scripts/PlayerMeleeSelection.lua`: `worldGlobals.PlayerMeleeSelection(targetId, hull, distance, seenAngles, heightDiff, playerWeapon, reach, special, friendly)` returns an animation name, a relative placement (`pmlRelPlacementInFront(d)`) and a show-weapon flag. Native caller **0x1404208f0** (string ref 0x140420bef). `pmlUseLongPlayerMelee` 0x14041f9b0 writes 0x142285bb0 |
| Melee-kill heal | script `HealthRestoreOnMeleeKill` 0x140464b20 (used by `HealthAmmoRestore.lua`) |
| `EDamageType` values (table 0x141628af8) | DMT_CUT 2, PUNCH 4, BLUNT 5, KICKING 7, SLEDGEHAMMERSWING 36, SLEDGEHAMMERSMASH 37, **PLAYERMELEE 38**, BLACKHOLE 42 |
| Weapon fire-mode enum | WFM_PRIMARY 0, SECONDARY 1, **WFM_MELEE 2** (table 0x141713018) |
| Unused in content | `CSledgeHammerWeaponEntity`, `CBaseballBatWeaponEntity`, `CChainsawWeaponEntity`, `CHammerWeaponEntity`, `CKnifeWeaponEntity` (vtables 0x141ad1fb0, 0x141a95d28, 0x141aa98a8, 0x141ab1cb8). No .ep among the extracted databases references them. `FRPCL_MeleeChainsaw.rsc` is a plain `CWeaponModeParams` of the chainsaw launcher (`FRPCL.ep`) |

## 2. Native melee hit detection

### 2.1 Knife: `CMacheteWeaponEntity` (vtable **0x141ab69b8**, class size 0x348)
Members: `en_timSwingStartTime` +0x310, `en_fNextSwingTestTime` +0x318, `en_eaEntitiesDamagedInCurrentSwing` +0x320,
`en_bThrowAnimInProgress` +0x330, `en_vThrowTargetPoint` +0x334. Params (`CMacheteWeaponParams`): `mwp_idSwingAnim`
+0x228, `…DualWielding` +0x230, `mwp_idThrowAnim` +0x238, **`mwp_fDamageRayRadius` +0x240, `mwp_slDamage` +0x244,
`mwp_fSwingRadius` +0x248, `mwp_fSwingAngularVelocity` +0x24c, `mwp_fSwingStartHeading` +0x250,
`mwp_fSwingDamageStartTime` +0x254, `mwp_fSwingDamageDuration` +0x258**, `mwp_idMaterialHitEffect` +0x260.

* Fire (`vt+0x8a0` = **0x1407900f0**): plays the swing animation, sets +0x310, and arms the callback
  `CMacheteWeaponEntity::OnSwingEnd` 0x140778fc0 → `vt+0xb08` 0x140790310 (clears +0x310 and the hit list).
* Step (`vt+0x208` = 0x140790230): when a swing is active it calls the **swing test 0x14078f3b0**. Inside the window
  [+0x254, +0x254+0x258] it steps a heading from +0x250 at +0x24c deg/s. For each step it casts **0x1401a4f20** (radius
  in result +0x98 = `mwp_fDamageRayRadius`, category CName "bullet" 0x14228dd08 twice, max distance
  `mwp_fSwingRadius`). Origin: player `vt+0xc30`/`vt+0xc28` (0x1402756f0 / 0x140478dd0). Orientation: player
  `vt+0xc08` (0x140478260). That is the **body or view, not the weapon model**, so in VR the hit arc ignores the hand.
* On a new hit entity (not yet in +0x320) it builds a `CDamage` (section 3): inflictor = player `vt+0x3e8`,
  `dmg_ulFlags |= 4`, type **2 (DMT_CUT)**, pre-check `target->vt[0x3f0](target,&dmg,2,hitObj)`, weapon index from
  weapon `vt+0x730`, amount = `mwp_slDamage × 0x1401ad2d0(target, xmm1)` (xmm1 = a local at [rbp+0x5c]; its meaning
  is unknown), hit point, direction, and hull part (result +0x60/+0x68 → dmg +0x80/+0x88, result +0x50 → +0x90).
  Then `target->vt[0x400](target, stepArg, &dmg)` and the material effect 0x1401ae970 (`mwp_idMaterialHitEffect`).
  **There is no authority check in the swing test**; the authority filter is in the damage apply (section 3).
* Throw (`vt+0x8b0` = 0x14078fc00): `en_vThrowTargetPoint` = player+0x2188 (aim target). The anim event "Thrown"
  (`vt+0x288` 0x1407900b0, CName 0x14228dd10) goes to 0x14078fd60, which calls `player vt+0x9d0` (0x140476340 →
  0x1402a3180) with the barrel placement turned toward +0x334, firing power 1.0 and launch velocity −1 (default).
  The projectile params are read at weapon params +0x268 (a member not in the reflection list: unknown name).

### 2.2 Generic melee: `CGenericMeleeWeaponEntity` (vtable 0x141aa1ef8)
The same structure: step 0x14077a840 → swing test **0x1407796d0** (ray 0x1401a4f20, "bullet" 0x14228d900, radius
`gmwp_fDamageRayRadius` +0x240). Damage type comes from `gmwp_dtDamageType` +0x268 and the amount from
`gmwp_slDamage` +0x244. Other params: `gmwp_fThrowHeight` +0x26c, `gmwp_fKnockUpRatio` +0x270.

### 2.3 Melee kills (`plcmdUse`)
Use action **0x140480810 / 0x140489100** (both are also called from the VR hand-use path 0x14043dcb0, R8). They pick
the hand weapon by `player+0x2504` (1 → +0xf90 right, else +0xf94), find a target with **0x14041ff20** (player view
`vt+0x340` mode 0, which is the head in VR, plus a view cone), then call **0x140473b40**(player, hTarget,
idMelee, iWeapon). That function, also behind script `DoUseMeleeAttack` 0x1404614e0, works as follows:
* On a client (`0x140bb3a90(net)`), it sends a net call (0x140bb3ad0, descriptor 0x142286920).
* On the authority (`0x140ba6e30(player)==0`), it calls `weapon->vt[0x8e0](weapon, target, iWeapon)`, may switch
  weapon (0x1404932b0), and raises "FriendlyMelee" on friends. It then sends a melee event (ctor 0x140250640) to the
  player state machine (`0x140ba6bd0`). Handler 0x1405d0c00 runs `PlayerMeleeSelection` (0x1404208f0, range/height
  globals 0x142248620/0x142248628) and starts the synchronized animation (latent object 0x1405d3190, 0x128 bytes).
The kill is canned: the player is placed at the returned relative placement. It is unsuitable as a physical-melee
backend.

### 2.4 Contact detector on every `CBaseWeaponEntity` (base vtable 0x1416b8888)
Members: `en_qvContactDetectorRelPlacement` +0x394, `en_pdContactDetectorPrimtive` +0x3b0 (`CPrimitiveDesc`),
`en_qvLastContactDetectorAbsPlacement` +0x3c0, **`en_fContactDetectorVelocity` +0x3dc**, `en_vContactDetectorVelocity`
+0x3e0. Params: `bwp_idContactDetectorCollisionCategory` +0x2c8, `bwp_pvfxFireVibration` +0x2a0.
* Base step `vt+0x208` = **0x14025e2b0** runs on every machine. It computes the detector placement **0x14025c0b0**
  (weapon placement `vt+0x9b8`, i.e. the hand-placed model in VR, ∘ +0x394), writes +0x3e0 = last − new and
  **+0x3dc = |Δ| / step time** (0x140ba7860), then stores +0x3c0.
* Contact query **0x14025ba40**(weapon, CName category): world primitive overlap **0x140d048b0**(world, &placement,
  weapon+0x3b0, category, &array) (→ 0x140d829b0 when world+0x120 is set). It returns the first hit whose class is
  0x142280d70 (name not resolved). The default category is params +0x2c8. This is a discrete overlap, not a sweep.
* Weapons on this base include Hands, ObjectHolder, StickyBomb-like classes, `CVRMeleeWeaponEntity` and most guns.
  The knife and generic melee derive from `CWeaponEntity` (no detector).

### 2.5 `CVRMeleeWeaponEntity` (vtable 0x141ae6790) is dead
Its step 0x1407d5e60 drives a physics blade (+0x438) and sizing, and fills `en_fNetAverageVelocity`. Its collision
handler `vt+0x160` 0x1407d5410 collects touched entities and deflects projectiles. It reads the "BladeBottom" and
"BladeTop" attachments. It does **not** apply damage: it only raises "VRMeleeWeaponEntityHit" (event 0x141ae63f8) for
script. Params `CVRMeleeWeaponParams` (`mwp_fRayLength` +0x2d0, `mwp_fRaysPerMeter` +0x2d4, …) have no content (R3).
Using it would need new params plus a script: not recommended.

## 3. Damage API and network authority

```
struct CDamage {                 // ctor 0x140175d80 (SM 0x140176990); reflection rec 0x141627638..
  CDamageInflictor dmg_diInflictor;  // +0x00: di_henInflictor +0, di_idCharacter +0x10, di_idCharacterClass +0x20,
                                     //        di_iAlignment +0x30, di_fDamageMultiplier +0x3c, di_ulFlags +0x40
  Vector3f dmg_vDirection;           // +0x48
  Vector3f dmg_vHitPoint;            // +0x54
  SLONG    dmg_slAmount;             // +0x60   (0x7fffffff = "kill", skipped by the ReceiveDamage script event)
  EDamageType dmg_dmtType;           // +0x64
  INDEX    dmg_iInflictorWeapon;     // +0x68
  FLOAT    dmg_fUpEnergyMultiplier;  // +0x6c
  IDENT    dmg_idWeaponCategoryOverride; // +0x70
  DamageFlags dmg_ulFlags;           // +0x78
  /* +0x80/+0x88 hit hull part, +0x90 ref: not in reflection; written by the knife from ray result +0x60/+0x68/+0x50 */
};
inflictor->vt[0x3e8](inflictor, CDamage*);        // fill the inflictor: puppet 0x1402a2a60 (SM 0x1402a2820)
target->vt[0x3f0](target, CDamage*, dmt, hitObj); // pre-check (player 0x14048d570, puppet 0x1402bc8b0; meaning inferred)
target->vt[0x400](target, ctx, const CDamage*);   // receive: puppet 0x1402bc670 (SM 0x1402bc460)
```
* `ctx` is the step's second argument, or `0x1411223e0()` in script wrappers.
* 0x1402bc670 raises the "ReceiveDamage" script event when it is enabled. Otherwise it calls the damage handler
  (params+0xe0 `vt+0x10`) or `vt+0x428`, which is **0x1402b57a0** for `CPuppetEntity` (SM 0x1402b55b0). That
  function changes health only `if (0x140ba6e30(puppet)==0 || puppet->vt[0x40]())`.
* **0x140ba6e30** returns 1 on a non-authoritative machine (world+0x1c0 → +0x230 net object `vt+0x70`/`vt+0x80`).
  So **native damage applied on a client is dropped; damage must be applied on the server/host.**
* Helper **0x14024cea0**(inflictor, hTarget, slAmount, iWeapon, CString typeName, &vDir, &vHitPoint) is the native of
  script `InflictDamageToTarget(WithPlacement)` (0x140248150 / 0x1402482c0). It has no authority check of its own.
* Client-to-server pattern (net object 0x140ba6b40, "is client" 0x140bb3a90, send 0x140bb3ad0, then the authority
  part guarded by 0x140ba6e30): used by `SetPlayingInVR` (R3), **0x140473b40** (melee, desc 0x142286920),
  **0x140492e90** (SetHeldGadget, desc 0x142286860) and **0x1404839a0** (OnHudCommand, desc 0x142286630, which also
  runs the HUD command locally afterwards).

## 4. Q2: the `vr_fMinSwingToHitSpeed` reader

* Class: **`CObjectHolderWeaponEntity`**. Class record 0x141abb8e0 (size 0x5b0, base `CBaseWeaponEntity`). Its
  constructor 0x1407934c0 stores vtable **0x141abcd20** (0x1407934d7). 0x14079ecd0 is at **+0x210**, a second
  per-step slot (the base has the stub 0x140044530); the step is `vt+0x208` 0x14079e8a0. The reader uses
  `en_henTarget` +0x408, `en_henPreviousTarget` +0x40c and `en_tmTargetAcquired` +0x410 ("TargetAcquired" /
  "TargetLost"). 0x141abc9a8 is the vtable of a 0x98-byte helper object (allocated in 0x140792f00, 0x140798b60).
* Logic (0x14079fa13 onward): if the owner has vrd (player+0x25c8) and `0x14025ba40(weapon,
  "object_holder_weapon_contact")` (CName 0x14228de48) finds a target and **+0x3dc > vr_fMinSwingToHitSpeed**
  (0x142249550), it fires once per contact (+0x54c). It sends `CDamage{amount 0x7fffffff, type 37
  SLEDGEHAMMERSMASH}` through `target->vt[0x400]` and the event "BreakDownBarriers" (0x1403d9e30), then a haptic
  pulse with `bwp_pvfxFireVibration` (+0x2a0) on vrd+0x3d8 (0x140442a10, `en_bRightHandWeapon` +0x118 ≠ 0) or
  vrd+0x4e0 (0x1404429d0). Desktop equivalent: the "Smash" anim event in `vt+0x288` 0x14079e1d0.
* **Is it a VR melee path for SS4 weapons? No.** It runs only for an object-holder weapon (carried objects) and only
  damages that category. Reusable parts for the mod: the speed field +0x3dc (any `CBaseWeaponEntity`, hand-placed
  in VR), the native damage call and the per-hand haptic helpers.
* SM: vtable 0x141aa1390, `+0x210` = 0x14079e360 (cvar 0x14222a000 read at 0x14079f0cf).

## 5. Cast API for the blade sweep (completes R8)

```
RayCast(res, world, const Vector3f* origin, const Vector3f* dir, CName catA, CName catB, float maxDist); // 0x1401a4f20 (SM 0x1401a5b00)
SegmentCast(res, world, const Vector3f* from, const Vector3f* to, CName catA, CName catB);              // 0x1401a4f70 (SM 0x1401a5b50)
core 0x1401a4590(res, world, maxDist) (SM 0x1401a5170); result ctor 0x1401a3f80 (SM 0x1401a4b60).
res+0x98 FLOAT radius (0 = thin ray; > 0 = sphere-swept; the knife uses mwp_fDamageRayRadius, AOE uses
wmp_fTrajectoryCollisionSize); res+0x9c flag (AOE sets 1 after the first segment; meaning unknown);
res+0x74 = 3 ray / 1 segment; res+0x88/+0x90 categories ("" → "bullet").
```
Category CNames seen: "bullet" (knife and generic melee), "object_holder_weapon_contact", "aoe_aim_ray"
(0x1422935d0), "camera_aim_ray", "use_aim_ray", "bullet_no_shootable_ghost" (R8). A world-only category was not
identified: classify the hit by result +0x08 (entity).

## 6. Gadgets: content and classes

| gadget | item (.ep, `CGadgetItemParams`) | weapon modes (params class) |
|---|---|---|
| BlackHole, NerveGas, Decoy, MiniNuke | `Items/GadgetItems/{BlackHole,NerveGasGadgetItem,DecoyGadgetItem,MiniNuke}.ep` | `*TargetingWeaponMode.rsc` (`CAOETargetingWeaponModeParams`; MiniNuke `CMiniNukeTargetingWeaponModeParams`) + `Launch*WeaponMode.rsc` (`CLaunchGadgetProjectileWeaponModeParams`) |
| SeriousTime (slomo) | `SeriousTime.ep` | `CActivateSeriousTimeWeaponModeParams` |
| CompanionDrone | `CompanionDroneGadgetItem.ep` | `CSpawnCompanionDroneWeaponModeParams` (`wmp_fInitialSpeed` +0xbc) |
| SeriousRage | `SeriousRageItem.ep` | `CActivateSeriousRageWeaponModeParams` / `CActivatePowerUpWeaponModeParams` |
| MedKit, ExtraLife | `MedKitItem.ep`, `ExtraLifeGadgetItem.ep` | `CHealingWeaponModeParams`, `CExtraLifeGadgetWeaponModeParams` |
| SM ArtilleryCall, Hoverboard | `SeriousSamSM/Databases/Items/Gadgets/{ArtilleryCall,HoverboardGadget}.ep` | AOE targeting + `CLaunchGadgetProjectileWeaponModeParams` (Hoverboard spawns `Hoverboard_DecoyBase.ep`) |
| ClusterBomb | only `Models/Gadgets/ClusterBomb/*` | **no item or mode: cut content** |
| "Detonator" | not a gadget: the `CStickyBombWeaponEntity` weapon (`StickyBombWeapon.ep`, `Detonator_VR.mdl`), section 12 | |

`CGadgetItemParams` (rec 0x141c1cfa8): `gip_idShootingAttachment` +0xf8, `gip_pwrWeaponRenderingParams` +0x100,
`gip_pwrWeaponRenderingParamsVR` +0x108, `gip_pctTool` +0x110 (`PlayerGadget_*_R.ctl`, third-person tool, right
hand), **`gip_pmcFirstPersonModel` +0x120** (`*_Arms.mdl`), anims `gip_idActivateAnim` +0x128 / `Idle` +0x130 /
`Recoil` +0x138 / `Sprint` +0x140, `gip_idPoseAdjusterPose` +0x148, `gip_idPoseAdjusterPoseVR` +0x150,
`gip_apwmpWeaponModeParams` +0x178, `gip_bIsManuallySelectable` +0x1a0, `gip_slMaxGadgetCount` +0x1a4. `*_Item.mdl`
is the pickup model.

`CAOETargetingWeaponModeParams`: `wmp_prepTrajectoryParams` +0xb8, `wmp_fTrajectoryCollisionSize` +0xc4,
`wmp_pgrFirstPersonAimToLaunchAngle` +0xd0, `wmp_vTrajectoryStartOffsetFirstPerson` +0xe0, … ;
`CLaunchGadgetProjectileWeaponModeParams`: `wmp_pgppProjectile` +0xa8.

## 7. Selection and holding

* Held gadget = **player+0x1f58** (`pwcs_iHeldGadget` in the crossing state), stamp +0x1f5c.
  * **0x140492e90** SetHeldGadget(player, gadgetParams): net call, then on the authority +0x1f58 =
    `0x140966480(inventory, params)`.
  * **0x140496d80**(player, next, idx) cycles (cmds 0xd/0xe) and writes +0x1f58 on the authority.
  * The wheel commit 0x1401270d0 → 0x140492e90 (R3).
* HUD command: `OnHudCommand` (player `vt+0x11b0` 0x1404839a0) → HUD 0x1400d7880 → gadget HUD element
  **0x1400c8160** (accepts 9 and 0xc..0xe; slow-mo 0x1401b9150(player,4)) → **0x1404835f0**(player, cmd):
  * The GadgetHolder is looked for **only in the right slot +0xf90** (class 0x14228d840).
  * **0xc QuickUse:** if the holder is up and not quick-use (+0x390 == 0) → holder `vt+0x8a0`. Otherwise the
    authority sets player+0x269c |= 8 and calls 0x140495b40(player, −1).
  * **0xd/0xe:** cycle; if the holder is up, bring it up again with the new gadget.
  * **9 (GadgetSelection)** and others → 0x140495b40(player, −1).
* **0x140495b40** brings up the GadgetHolder: weapon index of `ggp_pwpGadgetHolder` (game params +0xc60) →
  0x1404932b0(player, idx, …, **hand = player+0x2504**, 0) + slow-mo 0x1401b9150(player,5).
* 0x1404932b0: hand ≠ 0 → right slot, 0 → left slot; it then calls 0x1404938b0(player, right, left).
* While the holder is up, gadget `vt+0x8b0` (0x140776220) puts it away (0x140495b40(·,−1)).
* `CGadgetHolderWeaponEntity` (class name 0x141a9c910, base `CGenericWeaponEntity`; vtable **0x141a9d040**, ctor
  0x140775700; allocation 0x3a0 in 0x140775bc0). Members: `en_pctCachedTool` +0x370, **`en_bGadgetUsed` +0x378**,
  **`en_pgipUsedGadget` +0x380**, **`en_bQuickUse` +0x390**. Init (`vt+0x110` 0x140776240) takes the gadget and
  quick-use flag from the creation data +0x98/+0xa0.
  * Step 0x140776aa0: quick-use auto-fires.
  * Use (`vt+0x8a0` **0x140776550**): consume (0x140473e60 → 0x14041d360), recoil anim (`gip` +0x138), +0x378 = 1,
    `0x14077ffa0(holder,"Fire")`, state 8, callback "CWeaponEntity::OnRecoilEnd".
  * Anim events `vt+0x288` 0x140776310: "Discard" (drop the tool) and "RecoilEnd".

## 8. Targeting and launch (gate R6)

Weapon-mode objects (main vtables): **AOE targeting 0x141c78618**, **Launch 0x141c829f0**, base `CWeaponMode`
0x141c74630. The holder finds the targeting mode in its slot +0x328 (0x140775da0, class 0x1422935b8).
1. **Targeting step** (`vt+0x28` **0x140a62220**). Start = player view `vt+0x340` mode 0 (**the head in VR**) ∘ the
   mode's first-person offset (`wmp_vTrajectoryStartOffsetFirstPerson`) or the third-person one, chosen by holder
   `vt+0x818`. Launch angle = view pitch through `wmp_pgr*AimToLaunchAngle`. Speed = projectile params +0x6c
   (`0x14077da50(holder)` → params). It marches a ballistic arc with **SegmentCast 0x1401a4f70** (radius
   `wmp_fTrajectoryCollisionSize`, category "aoe_aim_ray" when projectile params +0xac > 0), up to 20 segments, or
   casts a straight **0x1401a4f20** ray. Stored results: **+0x30 valid, +0x34 launch QuatVect (quat + start),
   +0x50 speed, +0x54 time to hit**. It also draws the arc and marker (+0x20/+0x28).
2. **Launch activation** (`vt+0x50` **0x140a6f3e0**): validates the target ("ETRSMessages.GadgetTargetTooClose").
   It copies the targeting quat → **+0x20..+0x2c**, start → +0x30..+0x38, speed (0x140529120 = targeting +0x50) →
   **+0x3c** and time (0x140a61a20 = +0x54) → **+0x40**.
3. **Anim event** "Throw"/"Fire" (`vt+0x180` 0x140a6f820, CNames 0x142293790/0x142293798) → **0x140a6f5a0**. That
   function runs **only on the authority** (0x140ba6e30). Spawn placement = **barrel 0x1402d7220**(holder, …,
   `gip_idShootingAttachment`), i.e. the hand-placed model in VR. Properties (`CGenericProjectileProperties`, ctor
   0x140556da0, base `CBaseProjectileProperties`):
   * `bpp_fFireingPower` +0x80 = 1;
   * **`bpp_fLaunchVelocity` +0x84 = mode+0x3c**;
   * **`bpp_vLaunchDirectionOverride` +0x104 = −Z of the quat at mode+0x20**;
   * `bpp_fLifeTimeOverride` +0x114 = mode+0x40 (unless projectile params +0x68 & 0x20);
   * inflictor through player `vt+0x3e8` (+0x90).
   It then creates the entity with 0x140b9eb60(props, world).
4. **Projectile init** (`CBaseProjectileEntity vt+0x110` **0x14053ee30**): direction = +0x104 if it is non-zero,
   else the placement's −Z. Speed = +0x84, or params +0x6c when −1, **clamped to 250**. It optionally adds the
   spawner's velocity (owner `vt+0x290`, params +0x88 flag), then `vt+0x4c8` sets the linear velocity
   (`en_fLaunchVelocity` +0xa4). Generic spawn helper: **0x1402c4e70**(puppet, props, &params, &placement,
   firingPower, launchVelocity, henImmediateHit, iWeapon), authority only (0x1402a3180 adds the body→origin block
   check).

**R6 verdict: the native launch accepts an initial velocity** (direction + speed, both explicit) and a lifetime.
The mod can overwrite Launch mode **+0x20..+0x2c (quat whose −Z = release direction), +0x3c (|v|) and +0x40**
(lifetime) after `vt+0x50` and before the event. Alternatively, overwrite the targeting result (+0x34/+0x50/+0x54)
after its step. Both are fields of a native object; no gameplay state is duplicated.

## 9. Activated gadgets
* SeriousTime: event "Activate" (`vt+0x180` 0x140a5f6c0) → 0x140a5f240 spawns the time effect at the player's
  placement (`CSeriousTimeParams` at mode params +0xa8) and the tracking effect +0xb0.
* CompanionDrone: event "Throw" (0x140a746d0, CName 0x142293870) → **0x140a74400** (authority only) spawns the
  drone at the barrel placement. Its velocity = placement forward × `wmp_fInitialSpeed` (+0xbc), set through drone
  `vt+0x4c8`; there is no override field, so a hand-thrown drone would need a hook there.
* SeriousRage/power-ups: event "ActivatePowerUp" (base `vt+0x180` 0x140a5e0c0 → mode `vt+0x248`). MedKit/ExtraLife
  modes were not traced.
* All of them start from the same holder use (`vt+0x8a0`), so "trigger in hand" = OnHudCommand(·, 0xc).

## 10. Gadget model in the hand
* Model: holder `vt+0x668` = **0x140775e20** returns `gip_pmcFirstPersonModel` (+0x120, `*_Arms.mdl`). It overrides
  the base 0x1402d7f90, which picks the weapon VR model when +0x1f0 is set (R3), so **no VR-specific model is
  chosen**. `vt+0x650` 0x140775fc0 returns the cached tool +0x370. Pose adjuster `vt+0x718` 0x140775f30 = gip +0x148
  (non-VR). No reader of `gip_pwrWeaponRenderingParamsVR` (+0x108) or `gip_idPoseAdjusterPoseVR` (+0x150) was found
  in the holder code (field scan 0x140775000..0x140782000): unknown, probably unused.
* Placement: holder `vt+0x9b8` = **0x1402d8ad0** (the shared weapon placement). With vrd the model is placed at the
  controller chosen by `en_bRightHandWeapon` +0x118 (R3), so **it already follows the VR hand like any weapon**. The
  shoot placement `vt+0x9b0` = 0x14077d650 (generic). Whether `GadgetHolder.ep` sets `wp_bTwoHanded` +0x1b4 or
  `wp_dwrDualWieldRequirement` +0x1b8 is unknown (values not decoded).
* The `*_Arms` model contains Sam's first-person arms. How they look at a VR hand is a runtime check.

## 11. Network flow of a gadget throw (static)
1. Client: `plcmdGadgetQuickUse` → controller input (0x14040da90 region) → player `vt+0x11b0` OnHudCommand(·, 0xc).
   This is a net call to the server; the client also runs the HUD command locally.
2. Server: the net call runs 0x1404835f0 → 0x140495b40 → GadgetHolder in the right hand. The weapon switch and
   entity replicate.
3. Server: the holder (quick use) fires → targeting and launch modes → the anim event → 0x140a6f5a0 spawns the
   projectile (authority only) → the projectile entity replicates.
4. The client cannot pass velocity or aim: OnHudCommand carries only (mode, cmd).
5. The server's spawn placement comes from **its** hand-placed holder, i.e. the VR client's hand poses received
   through `ClientActionVR` (R3 s4; mod option `native_vr_action`). Its targeting comes from its copy of the head.

## 12. Sticky bomb and detonator (native VR code)
`CStickyBombWeaponEntity` (vtable 0x141adb320; params `CStickyBombWeaponParams`: `swp_ghVelocityOverDistance`
+0x220, `swp_fThrowVelocityMultiplier` +0x230, `swp_fMaxThrowRange` +0x244, `swp_fMinDetonateAfterThrowDelay`
+0x250, **`swp_pmcVRDetonator` +0x258** = `Detonator_VR.mdl`).
* Throw 0x1407c6910: with vrd it calls **0x1407cd0d0**. That function builds the throw from a per-hand **velocity at
  vrd+0x488 (right, `en_bRightHandWeapon` ≠ 0) / vrd+0x590 (left)**, i.e. +0xb0 of the per-hand objects at
  vrd+0x3d8/+0x4e0, times `swp_fThrowVelocityMultiplier` (+0x230 of the params returned by weapon `vt+0x618`). The writer of these velocities was **not found** (unknown).
* Step 0x1407cb7a0: with vrd, detonation uses the VR detonator model (+0x360) and the vrd button test 0x14043e9d0
  (`(state & 5) == 1` on index vrd+0x28c), and runs 0x1407c74e0 (detonate all) instead of the anim path.
* So "Detonator" = sticky-bomb detonate. It is already VR-aware; `bomb_fuse.hpp` is not needed for it.

## 13. SamSM.exe 628492 ports
Ports by `port_list.py` (unique 24..48-byte signature) or by the vtable slot (named). "weak" = the longer signature
did not match: confirm before use.

| function | Sam4 | SamSM |
|---|---|---|
| weapon placement `vt+0x9b8` | 0x1402d8ad0 | 0x1402d8720 (slot) |
| knife vtable / step / fire / throw / "Thrown" / swing test | 0x141ab69b8 / 0x140790230 / 0x1407900f0 / 0x14078fc00 / 0x14078fd60 / 0x14078f3b0 | 0x141a9b028 / 0x14078fb70 / 0x14078fa00 / 0x14078f510 / 0x14078f670 / 0x14078ec80 (call at the end of the step) |
| generic melee swing test | 0x1407796d0 | 0x140778f90 |
| base weapon step / detector placement / contact query | 0x14025e2b0 / 0x14025c0b0 / 0x14025ba40 | 0x14025dec0 / 0x14025bd00 / 0x14025b690 |
| ObjectHolder vtable / `+0x210` / event | 0x141abcd20 / 0x14079ecd0 / 0x14079e1d0 | 0x141aa1390 / 0x14079e360 / 0x14079d8a0 |
| melee net / target find / selection | 0x140473b40 / 0x14041ff20 / 0x1404208f0 | 0x140474140 (callee of script 0x140463370) / 0x14041fff0 weak / 0x140420990 weak |
| CDamage ctor / inflictor `vt+0x3e8` / receive `vt+0x400` / apply `vt+0x428` / inflict-to-target | 0x140175d80 / 0x1402a2a60 / 0x1402bc670 / 0x1402b57a0 / 0x14024cea0 | 0x140176990 / 0x1402a2820 / 0x1402bc460 / 0x1402b55b0 / 0x14024cac0 |
| ray / segment / core / result ctor / hit effect | 0x1401a4f20 / 0x1401a4f70 / 0x1401a4590 / 0x1401a3f80 / 0x1401ae970 | 0x1401a5b00 / 0x1401a5b50 / 0x1401a5170 / 0x1401a4b60 weak / 0x1401af4c0 |
| is-client (`0x140ba6e30`) | 0x140ba6e30 | 0x140ba7510 weak |
| vrd haptic right (+0x3d8) / left (+0x4e0) | 0x140442a10 / 0x1404429d0 | 0x140442af0 / 0x140442ab0 |
| SetHeldGadget native / OnHudCommand / gadget cmd / HUD element cmd | 0x140492e90 / 0x1404839a0 / 0x1404835f0 / 0x1400c8160 | 0x140493580 (callee of script 0x1404621b0) / 0x140483f20 (player vt 0x14181aea0 +0x11b0) / 0x140483b70 (callee of 0x1400c8320) / 0x1400c8320 weak |
| bring-up holder / cycle / select in hand | 0x140495b40 / 0x140496d80 / 0x1404932b0 | 0x140496240 weak / 0x1404973e0 weak / 0x1404939a0 weak |
| GadgetHolder vtable / use / step / model / init | 0x141a9d040 / 0x140776550 / 0x140776aa0 / 0x140775e20 / 0x140776240 | 0x141a816b0 / 0x140775e10 / 0x140776360 / 0x1407756e0 / 0x140775b00 |
| Launch mode vtable / `+0x50` / `+0x180` / launch | 0x141c829f0 / 0x140a6f3e0 / 0x140a6f820 / 0x140a6f5a0 | 0x141c67260 / 0x140a6f940 / 0x140a6fd80 / 0x140a6fb00 weak |
| AOE mode vtable / step `+0x28` | 0x141c78618 / 0x140a62220 | 0x141c5ce88 / 0x140a62640 |
| SeriousTime / drone spawn | 0x140a5f240 / 0x140a74400 | 0x140a5f690 / 0x140a74970 |
| projectile init / spawn / player launch `vt+0x9d0` / create entity / props ctor | 0x14053ee30 / 0x1402c4e70 / 0x140476340 / 0x140b9eb60 / 0x140556da0 | 0x14053f0a0 / 0x1402c4c70 / 0x140476960 / 0x140b9f190 / 0x140556fd0 |
| barrel / slow-mo | 0x1402d7220 / 0x1401b9150 | 0x1402d6e40 (= R8) / 0x1401b8c90 |
| `vr_fMinSwingToHitSpeed` | 0x142249550 | 0x14222a000 |

Member offsets used here (weapon, params, gadget, CDamage, projectile properties) come from reflection and are the
same in SM wherever they were checked. The player offsets shift by 8 (R8). Not ported: `player+0x1f58` in SM (it is
probably +0x1f60, given the +8 shift, but **unverified**), and the sticky-bomb VR throw.

## 14. Unknowns and runtime checks
1. The writer of the vrd per-hand velocity (vrd+0x488/+0x590), and whether it is live on our device.
2. `0x1401ad2d0`'s second argument; DamageFlags bit 4; the meaning of the pre-check `vt+0x3f0`; the class
   0x142280d70 the contact query accepts.
3. GadgetHolder `wp_bTwoHanded`/`wp_dwrDualWieldRequirement`, and whether 0x1404932b0 accepts it in the left slot.
   How the `*_Arms` model looks at a VR hand.
4. Whether the AOE targeting step runs on the server for a remote player, so the server's mode+0x20.. is fresh.
5. Whether a melee kill (synchronized animation) moves the VR camera; whether 0x14043dcb0 triggers it on our
   button mapping.
6. The SS4 knife's source (level content); the weapon mass needed by `melee_sim` (no `wp_` mass member exists:
   use per-class constants).
7. `ClientActionVR` rate on the server (it bounds the server's swing and throw sampling).

## 15. Recommended implementation

### Physical melee (Phase 5)
1. **Which weapons.** The knife (`CMacheteWeaponEntity`), generic melee items, and hands (fists). Optionally, the
   butt of any held weapon. The drawn weapon is the `melee_sim` pose. On the VR client, send that pose as the
   controller pose in `ClientActionVR` (`native_vr_action=1`) while a melee weapon is held. The engine then places the
   weapon there on every machine (0x1402d8ad0), and the server's vrd (+0x348/+0x364) has the simulated pose.
2. **Client (feedback only).** Each sim step, sweep the blade (`melee_sim.blade()` prev → cur, 3..5 points) with
   **SegmentCast 0x1401a4f70** (result ctor 0x1401a3f80, res+0x00 = owner handle, **res+0x98 = radius** = the
   knife's `mwp_fDamageRayRadius` or a default, category "bullet"). Feed world hits (result +0x08 not a puppet) to
   `MeleeContact` (stop, no tunnelling). Pulse the hand haptic with 0x140442a10 / 0x1404429d0 (right/left; vibration
   asset such as `bwp_pvfxFireVibration` where the params have one). No damage on the client: 0x1402b57a0 would
   drop it anyway.
3. **Authority (server or SP host, mod required on the server).** In a hook after the owner's weapon step (knife
   `vt+0x208` / base 0x14025e2b0), for players with vrd:
   * Take the blade from the weapon's placement this step versus the last step. The server's copy comes from the
     replicated pose, so it cannot be forged beyond what aim already allows.
   * Gate on speed > `vr_fMinSwingToHitSpeed` (0x142249550, the native cvar; the ObjectHolder reader in section 4 compares +0x3dc with it the same way) and on
     reach (hand within arm length of the head, scaled by `vr_fWorldScale`). Reject speed jumps (teleporting hand),
     apply a per-target cooldown and per-swing dedup (like `en_eaEntitiesDamagedInCurrentSwing` +0x320). Log
     rejections.
   * For each new puppet hit, apply damage **exactly like 0x14078f3b0**: `CDamage` ctor 0x140175d80; player
     `vt+0x3e8`; `ulFlags |= 4`; type `DMT_CUT` (knife), `gmwp_dtDamageType` (generic) or `DMT_PLAYERMELEE` 38
     (fists); amount = the params' `*_slDamage` (optionally scaled by speed); weapon index = weapon `vt+0x730`;
     direction = blade velocity; hit point and hull part from the result; then `target->vt[0x3f0]` check →
     `target->vt[0x400](target, stepArg, &dmg)` → hit effect 0x1401ae970. Health, gibs and kill credit stay native.
   * Optional: knock-up/impulse via `dmg_fUpEnergyMultiplier` (+0x6c).
4. **Do not** use `DoUseMeleeAttack` / 0x140473b40 for swings (canned kill and teleport), nor
   `CVRMeleeWeaponEntity` (script-only hits). Keep `plcmdUse` melee kills as a separate button; check their camera
   behaviour at runtime.
5. **Knife native swing.** When the hand path is active, suppress the knife's canned arc so hits are not doubled:
   skip 0x14078f3b0 for VR owners, or keep the fire button off the knife. Keep the knife throw (`vt+0x8b0`): it
   already launches from the hand-placed barrel.

### Gadgets in hand (Phase 6)
1. **Select / hold.** Use the native wheel or 0x140492e90 (net call) for the held gadget; take it out with
   OnHudCommand(0, 9) → GadgetHolder (native RPC). Off hand:
   * Preferred: bring it up in the left slot via 0x1404932b0 with hand 0, i.e. set player+0x2504 = 0 around
     0x140495b40, on the authority. Also hook 0x1404835f0 (and 0x140495b40's right-slot test) on the server to accept
     +0xf94. Runtime check 3 decides whether the engine allows it.
   * Fallback: keep it in the native right slot and only flip its placement role (`en_bRightHandWeapon` +0x118) to
     the left controller.
   The model then follows the hand natively (0x1402d8ad0).
2. **Throwables** (BlackHole, NerveGas, Decoy, MiniNuke, SM ArtilleryCall/Hoverboard):
   * The client detects the throw with `grenade_throw.hpp` and sends OnHudCommand(·, 0xc) at throw start, inside
     its `LaunchTimeout`.
   * On the authority, the server mod estimates the release velocity from the received hand poses (the same
     estimator, run on vrd+0x348/+0x364 history; or vrd+0x488/+0x590 if runtime check 1 shows them live).
   * Before the "Throw"/"Fire" event it writes the Launch mode **+0x20..+0x2c (quat, −Z = direction), +0x3c (speed,
     the native clamp is 250) and +0x40 (lifetime: the predicted flight time, or the native value)**.
   * The native launch 0x140a6f5a0 then spawns from the hand-placed shooting attachment with that velocity. This is
     server-validated by construction (clamp the speed; reject when the hand is out of reach).
   * Log the landing error of `grenade_throw`'s prediction against the native projectile (PLAN verify).
   * Hide or ignore the head-based native arc preview (it is drawn from local values in 0x140a62220).
3. **Activated gadgets** (SeriousTime, SeriousRage, MedKit, ExtraLife): squeeze → OnHudCommand(·, 0xc) → holder
   `vt+0x8a0`. Fully native, with no velocity. CompanionDrone: same; a hand-thrown drone would need a hook on the
   velocity passed to drone `vt+0x4c8` in 0x140a74400 (optional).
4. **Detonator:** use the native sticky-bomb VR path (section 12). Verify the hand-velocity source and the
   detonate button index (vrd+0x28c) on our device.
