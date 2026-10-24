# Gate R3: live VR gameplay in Sam4.exe 616154

Static analysis only (Ghidra project `sam4`/`fusion`, no game run). Sam4.exe addresses are PE VAs, and Fusion
(`Sam2017_VR.jotn-vr-stock`) addresses are ELF VAs. "vrd" means the `CPlayerVRData` object. "player" means a
`CPlayerActorPuppetEntity`. A vtable slot is written `vt+0xNN`. Offsets come from the engine's own reflection
records (SE4 meta member records, 0x60 bytes each: +0x20 name, +0x38 byte offset, +0x40 flags; decoded by
`tools/meta_members.py`) or from decompiled code at the cited address. Anything else is marked unknown.

New tools (all read-only): `tools/strrefs.py` (string → code/data refs), `tools/meta_members.py`
(member name → class/offset), `tools/cvar_readers.py` (functions referencing a cvar/global), `tools/callers.py`
(direct E8/E9 callers + data pointers), `tools/vcall_sites.py` (`call [reg+disp]` sites), `tools/field_refs.py`
(`[reg+disp]` accesses, read/write), `tools/elf_callers.py` (Fusion callers). Note: `riprefs.py` also matches
`disp+k` for instructions with k immediate bytes, so `cvar_readers.py` can list a neighbouring global's sites.
The vr_fVROffsetFromHandsX / vr_fVRWeaponPitchAdjustment lists below were re-checked by decoding the instruction.

## 0. Summary

* **The whole VR gameplay layer (`CPlayerVRData`, hand-placed weapons, muzzle shots, head-tracked camera,
  anchor turning, replication of hand/head poses, VR weapon wheel) is still compiled in and reachable.** Almost
  all of it depends on one per-player object, `player+0x25c8` (`CPlayerVRData*`, returned by script
  `CPlayerActorPuppetEntity::GetVRHandler` 0x140462000).
* That object is created only when the player's `psd_bIsPlayingInVR` is set at puppet init. SS4's only native
  setter call passes a **constant 0** (Fusion passes 1). Nothing in content calls the `SetPlayingInVR` script.
  So the layer is **dormant-callable**: calling the native setter with 1 before the player spawns turns it on.
* Many VR branches are also gated on `IsTalos()` (title == `Content/Talos.gtitle`), which is always false in SS4.
* The local player controller always sends the **desktop** `ClientAction` RPC. The `ClientActionVR` sender
  exists but has no caller in SS4 except a bot path. Hand and head poses still replicate through the entity
  state stream (`en_qvNet*Placement`), from whichever machine computes vrd.

## 1. VR-mode gate on the player

| item | address / evidence |
|---|---|
| `CPlayerSimulationData::psd_bIsPlayingInVR` | +0x98 (meta rec 0x141e37900, flags 0x4000); +0x9c is the change stamp |
| script `CPlayerSimulationData::SetPlayingInVR(BOOL bPlayingInVR)` | 0x140bdbd40; sends a net call (0x140bb3ad0, descriptor global 0x1422d58d0), then writes psd+0x98 / +0x9c locally |
| native `CPlayerSimulationData::SetPlayingInVR(psd, int)` | **0x140bdc6f0**: same net call, then the same write |
| only native caller | 0x140067980 (in vtable at 0x141590320, binds the local user to its psd): `xor edx,edx` at **0x140067b24**, `call 0x140bdc6f0` at 0x140067b29. That is **always false**. |
| Fusion equivalent | 0xe91060 calls 0x1e813a0(psd, **1**) at 0xe91169; Fusion `prjIsVRActive` 0xe8e620 = `vt[0x18](this,1)` |
| `prjIsVRActive`/`CorIsAppVR`/`IsAppVR` | 0x140062670: constant false (R1) |
| `CPlayerVRData` creation | player init **0x14042a390**: if `*(player->params(+0x540) + 0x950)` (`CPlayerPuppetParams::ppp_pvrsVRParams`, meta rec 0x1418582a8) ≠ 0 **and** `psd(0x140bcb7e0)->+0x98` ≠ 0, then `alloc(0x738)` (0x14042a4a9), ctor 0x140439120, `player+0x25c8 = vrd` (0x14042a4c8), init **0x14043e330**(vrd, player). `PlayerPuppet.ep` references `Databases/Puppets/PlayerVRParameters.rsc` (`CPlayerVRParams`), so +0x950 should be set (runtime check pending). |
| `CWeaponEntity::en_bOwnerPlayingInVR` | weapon+0x1f0 (meta rec 0x141711430) = `owner+0x25c8 != 0`, or psd+0x98 when there is no owner; written at 0x1402d4e20, 0x1402ddb7b (fn 0x1402dda60), 0x1402e1690 |
| `CWUCIsPlayingInVR` | world-unit condition class (meta only); `mdGetNameScript` shares stub 0x1404a61f0; no content use found |
| title predicates | `IsTalos` 0x1400a8cc0 (ident 0x142274910 ← "Content/Talos.gtitle", 0x140005b28), `IsTLH` 0x140aa7a80 (0x142295568), `IsSS3` 0x140aa7910 (0x142295550); SS4's own ident is 0x142295558. All are false in SS4. |

Can it be set at runtime? Yes. Call 0x140bdc6f0(psd, 1) on the simulation thread, or patch the constant at
0x140067b24 (a payload hook is preferred to a byte patch). It must happen **before** the player puppet is
initialised (0x14042a390 reads the flag only there; vrd is never created later). Re-spawn or level load picks
it up. Because psd+0x98 replicates (flag 0x4000) and the setter is a net call, every machine creates a vrd for
that player. The vrd's "local" flag is `vrd+0x328 = player->vt[0x670]()` (0x1402acea0), so remote copies use
replicated poses (section 4).

## 2. Aiming and weapons with a VR player

### CPlayerVRData placements (all world space, P7 = quat xyzw + pos)
* Anchor (play-space origin in world): `vrd+0x2c0` P7, read by script `GetVRAnchorLookDirEul` 0x140436d10.
  `0x140443c50`(out, player) returns it plus the unnamed globals 0x142285e64/68/6c (or mixed-reality anchor
  0x14043af00 when device+0x18 == 3). Updated each tick from the player body by 0x14043f180.
* **Controller world placement `0x14043b920(vrd, out, role)`** (role 0 = right, 1 = left; called by
  `GetControllerPlacementLeft/Right` 0x140436ad0/0x140436b20). If `vrd+0x328 == 0` (remote) it returns the
  replicated pose at `vrd+0x348 + role*0x1c`. If the device is null it returns the anchor. If
  `0x140050860()` is false (device API 4 "Emulated" without vr_bEmulatedVRControllers/vr_bTestBotIsPlaying,
  or vr_bVRControllerExclusiveInput off and input profile ≠ "VRController") it builds an **emulated hand** from
  the weapon model offset + head + `vr_fVROffsetFromHandsX/Y/Z` (0x1422aacf0/f4/f8, sole reader
  0x14043bcc0..0x14043bcd2). Otherwise it uses **device slot 0xb8(role)** (override pose at
  `vrd+0xa4+role*0xd0` when `vrd+0xa0+role*0xd0` ≠ 0) with prj_bVRSmoothViewY damping, composed with the anchor.
* Head world placement `0x14043cb50(vrd, out)` = anchor ∘ local head `0x14043d750` (device **slot 0x80**,
  cached at vrd+0x680, override flag vrd+0x69c, remote copy vrd+0x32c). Script `GetHeadPlacement` 0x140436c60,
  `GetLookDirEul` 0x140436cb0, Euler helper 0x14043cf10.
* `vr_fVRWeaponPitchAdjustment` (0x1422aacb0): **no code reader** (dead). `vr_fSteamVRPitchAdjustment`
  0x1422aacac is used only in 0x140442f80 when device API == 1 **and** IsTalos.

### Weapon world placement
`0x1402d8ad0` is a CWeaponEntity virtual (present in 41 weapon vtables). Owner = `handle(weapon+0x98)`:
* owner+0x25c8 == 0 → desktop view-model placement.
* owner+0x25c8 ≠ 0 → **controller placement** `0x14043b920(vrd, …, en_bRightHandWeapon(+0x118) == 0)`
  (mixed-reality eye 3 uses the copy 0x14043cf40). Two-handed weapons (weapon params +0x1b4 and weapon flag
  0x20 at +0x168) blend both hands via 0x1402e2d70, using the blend time `CPlayerVRParams+0xc8`.
  vr_fWeaponScale / vr_fWorldScale are applied when !IsTalos.
* Weapon step `0x1402de9a0` does the second-hand grab for two-handed weapons: grip distance from weapon params
  +0x214 or `CPlayerVRParams+0xcc`, and player+0x2338 fire/grip bits.
* VR model configs: with +0x1f0 set, 0x1402d7f90 / 0x1402dd0a0 pick weapon-params VR models (+0xd8, +0xf0, +0xf8).

### Shot ray
* `CWeaponEntity::GetBarrelPlacement` (script 0x1402d1ab0) → **0x1402d7220**. If `+0x1f0 == 0` and not third
  person (`player vt+0xdf0`) and `vt+0x10c0 == 0`, it uses the view-based barrel 0x1402d7a40. Otherwise it uses
  the model barrel 0x1402d7360. Then 0x1402d6910 applies the local offset. In VR (+0x1f0) that is the weapon
  model attachment `weapon vt+0x9b8(&0x141713968)`. 0x1402d7220 has 51 direct callers (fire modes).
  **In VR the barrel origin and orientation come from the hand-placed weapon model.**
* Player aim ray: `player+0x2254` origin / `+0x2260` direction (script `GetAimRay` 0x1404639e0), written only by
  **0x1404a3670** from `player vt+0x340` mode 1 (+0xf3c) unless IsTalos. The view placements
  (+0xf20 mode 0, +0xf3c mode 1) are rebuilt in 0x14046cd40 through `vt+0x11c8` = **0x1404795a0**. With vrd,
  their orientation is the **head** Euler (0x14043cf10) and the position is the body eye. So the crosshair
  ray and target (+0x2188 hit point) follow the head. Whether each fire mode shoots along the barrel or turns
  toward the head-ray target is **not resolved per mode**. Readers of the aim ray: 0x14023ad60 (sight/laser
  model), 0x14023d990 (grappling hook), 0x140475be0, 0x1407cdd90, 0x140a49610.
* VR fire gate 0x14025a5d0: with +0x1f0 set, firing is blocked for weapon-params +0x218 seconds after
  bring-up, unless the unnamed global 0x1422738e8 is set.
* Melee-swing helper (section 5) and laser 0x1402e0390 (prj_bRenderVRLaserSight, mixed-reality skip cvars)
  are also VR-aware.

## 3. Camera, view and turning

* First-person camera: render-view **0x14048dad0** (player) takes `player vt+0xdf8` = 0x1402a42a0 → `vt+0x340`
  = **0x1404794a0** mode 0. With vrd this returns **head world placement 0x14043cb50** (anchor ∘ slot 0x80).
  Without vrd it returns body view +0xf20. The result is stored at player+0x1c08. When stereo rendering is in
  progress (device+0x1c ≠ 0, device+0xb44 == 0), each eye view = camera ∘ **slot 0x78** eye transform
  (0x14048dad0 lines with eye 1/2), with projection from 0x140b6ac70 (slot 0x70). Otherwise 0x140b6a8f0 builds
  the union frustum. **With the device present but no vrd (the current mod state), eye offsets are applied but
  head rotation and position are not.**
* `0x1401d9e40` builds the **cinema/2D-screen** eye view only: device-local head (0x80) ∘ eye (0x78), with a
  quad at viewer+0x54/+0x5c. It is called from VR view 0x1401dcbe0 when device+0x08 ≠ 0 (2D mode). It does not
  use the world camera.
* Turning is decoupled from head yaw. VR rotation handler **0x14044e880** (object with +8 player, +0x10 vrd) reads
  `vr_iRotationMethod` (0x1422862a8),
  `vr_iRotationController`, `vr_fStrafeThreshold` (0x142248728, snap threshold), `vr_fRotationFrequency`,
  `vr_fContinuousMovementDeadzone`, and device name "WindowsMR". Method 1 = continuous, 2 = snap; any other value
  = continuous only when trackpad locomotion is on (`vr_bTrackpadLocomotion` or the session setting at +0x68 of
  0x14024c680(player)). It applies the turn through 0x1404425e0, which
  adds to the **anchor yaw vrd+0x674** and rebuilds the anchor (0x140441e80). Head yaw stays HMD-relative.
* Movement direction 0x140442f80 → vrd+0x628 (vMovementFrontEul) / vrd+0x634 (relative move):
  `vr_iHeadOrientedStrafe` (0x14228629c) 1 = head, 2 = anchor + head yaw, 3 = Omni (vr_bEnableOmniMovement),
  0 = controller of the stick hand.
* `CVRCameraEntity` (cutscene camera): `GetHeadPlacement` 0x14071da00 → 0x14071dc20 = entity placement ∘
  slot 0x80; `OnStep` 0x14071d910. No gameplay dependence.

## 4. Networking

* `ClientAction` (record 0x141860e48): `(UBYTE ubFireButtonStates, Vector3f vLookDirEul, BOOL bLockOnAimAllowed,
  Vector3f vRelMove, Vector3f vClientPos, FLOAT fClientH, hen henClientFloor, hen henClientMover,
  UBYTE ubClientResetPosition, UBYTE ubClientResetLookDir)`. Impl `vt+0x1298` = 0x140470080.
* `ClientActionVR` (record 0x141861208, same flags 0x40400): `(UBYTE ubFireButtonStates, QuatVect
  qvTrackerPlacement, QuatVect qvControllerR, QuatVect qvControllerL, Vector3f vRelMove, Vector3f vClientPos,
  FLOAT fClientH, Vector3f vMovementFrontEul, hen henClientFloor, hen henClientMover, UBYTE ×2)`. Net stub
  **0x140470310** sends via 0x140bb3ad0, then on the receiver writes vrd+0x32c (head, device-local),
  vrd+0x348 (R, world), vrd+0x364 (L, world), vrd+0x39c (movement front). It derives
  `vLookDirEul = Euler(0x14043cb50 head world)`, so the look direction is head-driven. Both RPCs end in the same
  apply **0x140471680** (fire bits → player+0x2338, look → player+0x22c0 via 0x14049def0, move → +0x22f0).
  They are parallel RPCs in the same per-player action mechanism, so a desktop player (ClientAction) and a VR
  player (ClientActionVR) can share a server.
* Senders: `0x14048b730(player)` (VR, builds the args from vrd) is reached only through
  **CPlayerController vt+0x50 = 0x140415190** (controller vtable 0x14182e920). There is no `call [reg+0x50]` in
  the controller code, and the game loop 0x140058800 calls only controller vt+0x10/0x18. The bot/replay path
  0x14048aca0 (from 0x1404fc810) injects state (0x140440250 → device slots 0x88/0xc8/0xd8) and sends both.
  **The live local path is the alive-input controller vt+0x40 = 0x1404135c0 → player vt+0x1290 = 0x14048b160
  → ClientAction (or ClientAction_AimedPuppet 0x140470680).**
* State replication of vrd (independent of the RPC): player vt+0x60 = **0x14048a770** copies
  `0x14043c560/0x14043c540` (R/L world) and the head into the replicated members
  `en_qvNetRightVRControllerPlacement` +0x25d0, `en_qvNetLeftVRControllerPlacement` +0x25f4,
  `en_qvNetTrackerPlacement` +0x2618 (flags 0x4000). On copies where vt+0x670 == 0, vt+0x68 = **0x14048a410**
  writes them back into vrd+0x348/+0x364/+0x32c. So the authority's vrd poses reach every other machine.
  Caveat: with only ClientAction, the server's copy of a remote VR player has no source for those poses.
  Hand-placed weapons then work in single player / on the host, but not for VR clients, until poses travel
  (ClientActionVR or the mod's own channel).

## 5. Melee

* `CVRMeleeWeaponEntity` (class rec 0x141ae5360, size 0x488, base CBaseWeaponEntity, "VR melee weapon",
  member en_fNetAverageVelocity +0x468), script methods 0x1407d48a0..0x1407d4a90, hit event
  `CVRMeleeWeaponEntityHitScriptEvent` 0x1407d47a0/0x1407d4820. **No SS4 content references
  `CVRMeleeWeaponEntity` or `CVRMeleeWeaponParams`** (scan of all .ep/.rsc/.wld/.lua/.ctl in both .gro). NPC
  and test-bot .ep files contain only `CVRMeleeWeaponUsage` / `CVRBowWeapon*` (bot weapon-usage descriptors,
  `bwsp_pmwuVRMeleeWeaponsUsage` +0x50). The class is constructible only if the mod supplies weapon params:
  dead in stock content.
* `vr_fMinSwingToHitSpeed` (0x142249550) has one reader at 0x14079fa13, in fn 0x14079ecd0 = slot +0x588 of the
  vtable at 0x141abc9a8 (class not identified). If the owner has vrd and weapon+0x3dc (swing speed) exceeds
  the cvar, it raises "BreakDownBarriers" on breakable targets (class 0x14228de48) and pulses haptics through
  0x1404429d0 / 0x140442a10 (right/left, weapon params +0x2a0).
* `CVRGrenadeWeaponParams` is referenced by `PlayerVRParameters.rsc` (`pvp_pepVRGrenadeWeaponParams` +0x60),
  so the VR grenade has content. `CVRGrenadeWeaponEntity` class size is 0x430.

## 6. Weapon and gadget selection

### Native (desktop) wheel: reuse in VR
* HUD element `CWeaponSelectionWheelHudElement` (size 0x128, vtable **0x141604220**, ctor 0x140124a10,
  params class `CWeaponSelectionWheelHudElementParams`). `+0x08` = owning HUD (its +8 = player). Elements
  `he_awsweWheelElements` +0x40 (count +0x48); gadget slots array +0x50 (0x28 each, count +0x58).
* **Open/close.** Player flag `player+0x269c` bit0 = wheel open. The controller input fn 0x14040da90 handles
  `plcmdShowWeaponWheel` (handle 0x142285aa0). Holding it longer than the unnamed float 0x142248598 sets bit0
  (+ player+0xfa8 |= 0x10000000). A shorter tap calls `player vt+0x1180` = 0x140488ff0 (last weapon,
  0x140496a80). `plcmdWeaponSelectMod` (0x142285a78) press/release = 0x140489900 / 0x140489a20 (same as the
  scripts `OnWeaponSelectModPressed/Released` 0x14045bf30/0x14045c080). Wheel vt+0x90 = 0x140127d60 opens it
  (0x14012ba50: builds the lists, slow-mo `0x1401b9150(player, 2)`, opens the menu
  "CMSDummyWeaponSelectionController" with "WeaponSelectionHudElement") and closes it (**0x14012be40**).
  Visibility is wheel vt+0x60 = 0x140128290 (bit0 or fade +0xc0 > 0.05).
* **Selection direction.** Gamepad mode (wheel+0xb8 == 1): wheel vt+0x80 = **0x1401298c0** reads
  `syscmdH+/H-/P+/P-` (0x142275698..0x1422756b0) and `syscmdAltH±/AltP±` (0x1422756b8..0x1422756d0). If the
  vector length exceeds 0.75 (first) or 0.5 (second), it calls `0x140125b20(wheel, normalized float2, ring,
  hand)`. Ring change: mouse wheel events 0x34/0x35 (wheel vt+0xc0 = 0x140126ee0) and slot next/prev. The
  digital path goes through `player vt+0x11b0` = **0x1404839a0** (`OnHudCommand(mode, cmd)` net call → HUD
  0x1400d7880). Directions come from 0x14040cb30 (stick sectors → cmds 1..8 with mode 1) and 0x14040c880
  (`plcmdWeaponSlot1..8` 0x142285900..938). Slot next/prev are cmds 10/11 (`plcmdRight/LeftWeaponWheelSlotNext/
  Prev`). Script `CPlayerActorPuppetEntity::OnHudCommand` 0x14045c640 exposes it.
* **Highlighted item:** wheel+0x78 (primary hand) and +0x80 (second hand); last valid +0x98 / +0xa0; current
  weapons +0x68 / +0x70; gadget highlight +0x88 / +0x90; ring indices +0x60 / +0x64; dual-wield pending +0xbc
  (needs 0x140434290(player+0x19f8)). Element +0x48 = weapon params; index via 0x140965350(inventory, params).
* **Commit** happens on close (0x14012be40): `0x140470ee0(player, idx, hand)` (single) or
  `0x140471450(player, idxR, idxL)` (dual). The script equivalents are
  `ClientSelectWeaponInHand_WeaponWheel` 0x14045c800 / `ClientSelectWeaponsInHand_WeaponWheel` 0x14045c9c0
  (net call + 0x1404932b0 / 0x140493b70, which reset player+0xfc0/+0xfc8). Current right weapon:
  `player vt+0x1148` = 0x14047a0e0.
* **Gadgets:** `plcmdGadgetSelection` (0x1422859c0) → OnHudCommand(0, 9); QuickUse (0x1422859a8) → (·, 0xc);
  Next/Prev (0x1422859b0/0x1422859b8) → 0xd / 0xe. In the wheel, `syscmdUp/Down` (0x1422756d8/0x1422756e0)
  cycle the gadget ring and 0x1401270d0 → **0x140492e90(player, gadgetParams)** (also used by script
  `SetHeldGadget` 0x140460480).
* Render: wheel vt+0x80 draws into the HUD drawport (texts "ETRSHUD.WeaponWheel.*"; hud_fWeaponWheelScale,
  hud_slWeaponWheelElementSubdivision, hud_fWeaponWheelFirstRingSelectionRadiusHisteresis readers
  0x1401261e0..0x14012dc40). It is a 2D HUD element, so it lands on the HUD composition layer.
* Slow-mo parameters: `CPlayerController::SetSelectionWheelSlowMoRTF/Duration` 0x14040bf70 / 0x14040bff0 write
  controller+0x20c / +0x210..+0x21c.

### VR wheel remnants (`CVRWeaponSelectionWheelInfo`)
* Resource `Databases/WeaponWheelSelectionInfo.rsc` (`CVRWeaponSelectionWheelInfo`, size 0x208) is referenced
  by `PlayerVRParameters.rsc` (`pvp_presVRSelectionWheelParams` +0xb8). Its models
  (`Content/SeriousSamFusion/Models/Interface/*Wheel*.mdl`) **are present** in 00_All.gro.
  `CVRWeaponSelectionElementRenCmd` has a class record (0x141849dc8, size 0x90).
* Code is live: the object lives in **CPlayerController+0x1c8** (vrd finds the controller via 0x14043d300,
  cached at vrd+0x6d8). Active(hand) 0x140453ed0, initialised flag +0xa0 (0x140453d90), deactivate 0x1404577e0,
  per-tick reset 0x140454370 (end of 0x14040da90). Placement/render: 0x1404549d0, 0x140454cad, 0x140455142,
  0x140455506, 0x140456fa0 (`vr_fWeaponSelectionDepthDistanceFromHead` 0x140454d4d,
  `vr_fWeaponSelectionTooltipScale` 0x140456a19). It is driven from the vrd button logic in 0x1404399c0 /
  0x14043dcb0. `vr_vwsslWeaponSelectionLocation`, `vr_fWeaponSelectionHeightDistanceFromHead` and
  `vr_fWeaponSelectionStretchOverride` have **no readers**. Ability flag: vrd+0x2bc bit0
  (`EnableAbilityWeaponSelection` 0x1404381c0 → 0x14043ab50).

## 7. Null-dereference risks with the device global non-null

1. *(fixed)* viewport+0x360 VR viewer in the mouse handler (crash at 0x140053263).
2. **vrd init 0x14043e330 calls device slot 0x208 with no null check** (0x14043e87e / 0x14043e8b6). It runs for
   every player whose psd+0x98 is set, including **remote VR players on a machine whose device global is
   NULL** (a desktop peer with XR off) → crash. Slot 0x208 must return NULL on our device (the stub does).
3. **0x1401d8430 → `0x14043b920(player+0x25c8)` without a null check** (call 0x1401d869a). It is reached every VR
   frame from 0x1401dcbe0 (calls at 0x1401dd1cf / 0x1401dd25b) when device+0x08 == 0 or viewer+0xd0 ≠ 0, the
   view entity (0x1401d91d0) is a player, viewer+0xd0 ≠ 0 and viewer+0xb0 == 0. With vrd absent it reads
   [0+0x328]. It only returns early when the device name is "Emulated".
4. ClientActionVR stub 0x140470310 writes `player+0x25c8 → +0x32c..+0x3a4` with no check. A server that
   receives ClientActionVR for a player without vrd crashes.
5. Unguarded device calls that are safe only while the device exists: vrd init slot 0x110 (only if IsTalos);
   0x140442f80 slots 0x148 / 0x110 (local movement); 0x14044e880 slot 0x110; 0x14043cb50 reads device+0x18
   under the vrd+0x328 (local) condition. On a machine without a device these are reachable only for a
   **local** VR player.
6. Device base field +0xb68 (Omni) is dereferenced by 0x140b6ac00 / 0x140b6ab20 / 0x140b6ae30 when
   vr_bEnableOmniMovement or `vr_iHeadOrientedStrafe == 3`; keep it NULL and the cvars off.

## 8. PLAN R3 classification

| item | status |
|---|---|
| `SetPlayingInVR` (script 0x140bdbd40, native 0x140bdc6f0) | **dormant-callable** (native caller passes 0) |
| `ClientActionVR` (stub 0x140470310, sender 0x14048b730) | **dormant-callable** (no live sender; receiver live) |
| `CVRMeleeWeaponEntity` + HitScriptEvent | **dead in content** (class/code present, no params) |
| `CVRGrenadeWeaponEntity` | dormant-callable (params in PlayerVRParameters.rsc; activation path not traced) |
| `CVRWeaponSelection*` (VR wheel) | **dormant-callable** (needs vrd; resource and models present) |
| VR legs / `CIKStaticModelEntity::InitPlayerVRMovementHandler` 0x14061e790 | not traced in this pass |
| rotation/movement cvars | live once vrd exists (0x14044e880, 0x140442f80; trackpad/strafe/step readers listed by `cvar_readers.py`) |
| native ray/hull cast, damage | ray helpers used by the aim writer: 0x1401a4f20 / 0x1401acbc0 (signature not pinned); damage not traced |

## 9. Hooks and calls for the mod, in priority order

1. **Enable vrd for the local VR player.** Hook 0x140bdc6f0 (or the call at 0x140067b29) to pass 1 for the
   local user when an XR session exists, before the puppet spawns. This one change enables hand-placed weapons
   (0x1402d8ad0), barrel-from-muzzle (0x1402d7220/0x1402d7360), the head-tracked first-person camera
   (0x1404794a0 → 0x14043cb50 ∘ slot 0x78), anchor turning and pose replication. Make sure device slots 0xb8,
   0xc0, 0x80, 0x78 and 0xd0 return the OpenXR grip/aim and head poses in play-space metres ÷ vr_fWorldScale,
   and that device+0xb3c = 1. Otherwise the input profile is not "VRController" (game loop 0x140058800),
   0x140050860 returns false, and 0x14043b920 falls back to emulated hands.
2. **Desktop and mixed-MP safety:** wrap 0x14043e330 (and 0x1401d8430) so a NULL device global or a missing vrd
   cannot be dereferenced. Desktop peers keep the global NULL and get a temporary stub for slot 0x208 during
   0x14043e330, or the mod skips vrd creation for remote players there. Guard 0x140470310 on the server for
   players without vrd.
3. **Pose transport for VR clients:** either route the controller's alive-input send to the VR sender
   0x14048b730 when player+0x25c8 ≠ 0. That needs the native VR inputs: fire bits vrd+0x288, the button-index map
   vrd+0x28c..0x2a8 set in 0x14043e330, and device slot 0xd0. or keep ClientAction and supply vrd+0x32c/0x348/0x364 on the server from the mod's channel. Entity
   replication (0x14048a770 / 0x14048a410) then fans them out.
4. **Native weapon/gadget wheel:** open/close by setting/clearing `plcmdShowWeaponWheel` (virtual pad
   LeftShoulder hold) or by mirroring 0x140489900/0x140489a20. Feed direction through `syscmdH±/P±`
   (primary) and `syscmdAltH±/AltP±` (second hand) axis values from the hand or thumbstick, or call
   0x140125b20 and write wheel+0x78/+0x80. Read the highlight from wheel+0x78/+0x80/+0x88. The commit stays
   native (0x14012be40 → 0x140470ee0 / 0x140471450). Gadgets: OnHudCommand (player vt+0x11b0) cmds 9/0xc/0xd/0xe,
   or 0x140492e90. Render as-is on the HUD layer.
5. **Crosshair:** project along the barrel (0x1402d7220 result) for hand aim. The engine's own aim ray
   (player+0x2254/+0x2260 from 0x1404a3670) follows the head in VR; check each fire mode's use of +0x2188
   before relying on barrel-only firing.

## Open items
* Fire-mode direction: barrel forward vs. head-ray target (+0x2188), per weapon mode.
* `player vt+0x670` meaning (taken as "locally controlled" from use) and controller-vt+0x50 callers via
  non-vtable paths (none found).
* Class of the melee-swing consumer (vtable 0x141abc9a8). Unnamed globals: 0x142285e64..6c (anchor offset),
  0x142248598 (wheel hold time), 0x1422738e8 (VR fire-delay override).
* Runtime confirmation that PlayerPuppet params +0x950 is non-null in SS4.

## 10. Follow-up: what the mod does with this (static, not yet run)

Implemented in `src/se4/player_vr.cpp` (installed when VR is enabled; debug-skip bit 0x10 disables it):

* **VR player enable.** Detour on `set_playing_in_vr`. The value is forced to 1 only when the call returns to
  `bind_local_user_psd`'s own call site (found at runtime as the single `call rel32` to the setter; it is the
  12th direct call in all four builds) and our device is installed. Ported addresses: Sam4 0x140bdc6f0,
  Sam4_Unrestricted 0x140bdf2d0, SamSM 0x140bdce60, SamSM_Unrestricted 0x140bdfa30. The call-order port
  agrees with an independent 272-byte signature in every build. psd+0x98/+0x9c are the same in all builds.
* **Player → vrd offset differs per game:** SS4 `player+0x25c8`, **SM `player+0x25d0`** (from the
  `ClientActionVR` apply: `mov rcx,[rsi+disp32]` before the store to vrd+0x32c). The mod reads it from the
  running build's code rather than assuming SS4's value.
* **Guards:** `vrd_init`'s two slot 0x208 calls (16 bytes each, identical shape in all builds: Sam4
  0x14043e87e/0x14043e8ac, SM 0x14043e9ce/0x14043e9fc) become `call thunk` + NOPs. The thunk returns 0
  when the device global is NULL and otherwise tail-jumps to the slot. `vrd_controller_world` returns an
  all-zero placement for a NULL vrd, which makes the hand-ray caller 0x1401d8430 return. `ClientActionVR` is
  dropped for a player without vrd. The player can only get VR data after every guard is in place.
* **Input mode, and a bug this found:** the game loop (0x140058800, and 0x1400528a0) switches the active
  input device to the one named "VRController" whenever `device+0xb3c` ≠ 0. SS4/SM register no input device
  of that name; `CVRController` in .rdata is a metadata class, not an input device. The lookup
  (0x1412e0aa0) then returns −1. The device used to set +0xb3c on the first trigger or grip press, which
  would have dropped the virtual gamepad. It now never sets +0xb3c. Instead
  `vr_bVRControllerExclusiveInput` (only reader: 0x140050860) is forced to 1, so controller placement comes
  from device slot 0xb8 instead of emulated hands. It is restored when the engine shuts the device down
  (`cvars::restoreOverrides`).
* **Locomotion stays on the virtual gamepad.** The live local sender (controller vt+0x40 0x1404135c0, the
  "FPSControls" path) reads only plcmd axes. It never reads vrd, so pad movement and look still drive
  `ClientAction`. The native VR turn (0x14044e880) and VR move (0x140442f80) read the device stick (button
  table indices 2/3, per-hand fields vrd+0x14/+0xe4). The device reports those axes as 0 for now
  (`kNativeStickLocomotion`) to avoid turning twice. The anchor update 0x14043f180 follows the player body
  placement, so pad turning probably carries the anchor along. Not proven: check at runtime.
* `0x1401d8430` is the **native VR menu pointer**. It casts the hand ray onto the 2D screen quad and sends
  mouse move and press events to the viewport (vt+0x78 with 1/3/4/6/7) from buttons 0/1 (`0x1401dec10`).
  It is a candidate for Phase 8 menu interaction.

Runtime checks (headset or lab), in order:
1. The log shows `player_vr: vrd offset 0x25c8|0x25d0`, both slot 0x208 sites patched, and, on level start,
   `local user psd … marked as playing in VR` then `CPlayerVRData … (local 1)`.
2. The camera follows HMD position and rotation in the level. Weapons follow the right controller. Firing
   works.
3. Turn with the right stick and check whether the view yaw changes once (pad) or twice.
4. Check that the ini is unchanged after a clean exit (`vr_bVRControllerExclusiveInput`, FPS caps).

### 10.1 More follow-up findings
* Device slot 0x118 (controller type per role) decides how the engine reads the stick (0x140444a20).
  "Vive" means a trackpad that only counts while touched or pressed, and "WindowsMR" means the same unless
  `vr_bWindowsMRJoystickMovement` is set. "Knuckles" reads as a thumbstick unless slot 0x210
  (hand skeleton) is non-zero; "Rift" always reads as a thumbstick. The device now reports Knuckles,
  Rift, WindowsMR or Vive per hand from the OpenXR interaction profile. Slot 0x110 reports Rift,
  WindowsMR or Vive.
* `vr_iRotationController` (0x142248730) picks the turning hand in 0x14043ed10. Value 1 means role 1 (left),
  2 means role 0 (right), and 3 means both unless `vr_bStepMovement`. The per-hand stick fields are
  vrd+0x14 + role·0xd0 (x) and +0x18 + role·0xd0 (y).
* The VR locomotion handlers (two objects in vrd+0x278, ticked by 0x1404399c0) only run when
  `vrd+0x2bc & 0x18 == 0x18`. Those bits come from `CPlayerVRParams` +0xac. The handlers include the turn
  handler 0x14044e880.
* 0x14043dcb0 writes **player+0x2188 (target point)** and +0x2190/+0x2194 from the per-hand vrd entry
  (+0x5c + role·0xd0), and sets player+0x2504 = (role == 0). In VR the aim target therefore comes from the
  hand ray. See R8.
* The native VR sender 0x14048b730 and SM's 0x14048bd50 use the same vrd offsets (+0x288 fire bits, +0x628
  movement front, +0x634 relative move, +0x658). Player-side fields shift by 8 in SM (vrd pointer
  +0x25c8 → +0x25d0, +0x2308 → +0x2310).
* Mod option `native_vr_action=1` (ss4smvr/options.txt) turns the local VR player's desktop
  `ClientAction` into `ClientActionVR` with the same arguments plus head and hands. That is what lets a
  multiplayer server place a VR client's weapons. It needs `native_turn=1` (implied) because the look then
  follows the head. Both options are off until a headset test.
