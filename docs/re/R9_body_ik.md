# Gate R9: body IK application in Sam4.exe 616154 (ported to SamSM.exe 628492)

Static analysis only (Ghidra copy `lab/ghidra_r9/sam4`, PE scans, content scans of the `.gro` zips). Nothing
was run. Sam4 addresses are PE VAs. **SM** addresses come from a unique wildcarded signature
(`tools/sigscan.py`, 64 bytes unless a length is given) or from call order/vtable slots, as marked. "vrd" means
`CPlayerVRData`, "player" means `CPlayerActorPuppetEntity`, "renderable" means the player's model renderable
(`player+0x1e0`, returned by player `vt+0xc8` = 0x1402a7b00; SM `vt+0xc8` = 0x1402a78c0). Member offsets come
from SE4 reflection records (`tools/meta_members.py` / `meta_class.py`, record address given). Anything not
backed by an address is marked **unknown** or **inferred**.

New read-only tools: `tools/gro_extract.py` (list/extract `.gro` members into `lab/`),
`tools/meta_types.py` (which content files *serialize* an object of a class: DTTY table, not EXTY),
`tools/disasm.py` (annotated capstone listing), `tools/ghidra/CallerTree.java` (recursive caller tree).
Private dumps used here: `lab/r9/*.c` (decompiles), `lab/r9/content/` (extracted `.ep/.rsc/.skl/.mdl/.mtr/.lua`).

## 0. Summary

* **The engine has a complete native VR body system, but SS4/SM ship no content for it.** Two layers exist:
  (a) a *player VR movement handler* (room-scale body with leg/crouch animations, head and hand adjusters,
  weapon-hold anims) owned by vrd, and (b) a *head + hand adjuster pair on the third-person body* created right
  after vrd. Both need scheme resources (`CPlayerVRMovementParams`, `CSkeletonAdjusterScheme`,
  `CHandAdjustmentScheme`) that no SS4 or SM content file serializes. So both are **live code, dead in content**:
  with vrd enabled nothing changes on the body. The third-person body (seen by others and in mirrors) keeps
  playing desktop locomotion animations; only the weapons follow the replicated hands (R3 s2).
* **The native extension point for bone overrides is the `CMotionHandler` list of a model's anim queue.**
  After the animation blend, each registered handler's `vt+0x20` gets the per-bone *local* pose arrays
  (quat + position per bone) and may overwrite them. The engine registers handlers with
  **0x140bf6b20(renderable, handler, 0)** (SM 0x140bf7420) and removes them with **0x140bf7090** (SM 0x140bf7990).
  A generic scriptable handler, `CBonePathFollower`, also exists (per-bone desired placement in parent, model or
  world space, with strength). Either one is the recommended hook (section 5).
* **First person:** SS4 normally excludes the player's own body from its view (`0x140ca3280(player)` at
  0x14048ddd4) and draws the view-model arms and weapons only. A native "first-person avatar" path exists
  (`PNF_AVATAR_SEEN_IN_1ST_PERSON`, used by scripted `PlayFirstPersonAvatarAnim` scenes): it draws the body in
  first person with the model tag `FirstPersonAvatar`. That tag is a material condition on Sam's
  head/top/sleeves materials. There is no cvar for legs/body in first person.
* **New desktop-peer crash risk:** 0x14043aac0 calls device slot 0x1d8 with no null check. It runs every tick
  for every alive player with vrd (0x140486a40 → 0x1404399c0 → 0x1407228c0). That includes *remote* VR
  players on a desktop peer whose device global is NULL. The current guards (R3 s10) do not cover it.

## 1. What the engine does natively for VR bodies

| item | evidence | status |
|---|---|---|
| **Player VR movement handler** (object 0x110, ctor 0x140ac6470, init **0x140ac70c0**, update **0x140ac7710**; SM 0x140ac7580 / 0x140ac7bd0) | Created by **0x14043f070**(vrd) (SM 0x14043f1c0, 128-byte sig), called from player init 0x14042a390 at 0x14042a4f9. Created only if `(vrd+0x2bc & 0x18) != 0x18` **and** `CPlayerVRParams+0xd0` (`pvp_ppvmpPlayerVRMovementParams`, rec 0x14183bd68) ≠ 0. Stored at vrd+0x678. Init refuses without a head scheme ("Player VR movement handler initialization failed: head adjustment scheme is missing") and when other motion handlers are registered ("Make sure there are no other registered motion handlers"). Update from the vrd tick **0x14043f180** at 0x1404401a0 (SM tick 0x14043f2d0), with head world 0x14043cb50 and both controllers 0x14043b920. It sets the head skeleton adjuster (0x140946cb0) and the hand adjustment handler (0x140237950), blends leg anims by head height from `pvmp_pvlmaLegMovementAnimations` (+0x90; 0x140ac9390), changes model stretch by head height (0x140bfe0c0), and plays `pvmp_idWeaponHoldAnim_Right/Left` (+0xa0/+0xa8). | **dead in content**: `PlayerVRParameters.rsc` (both games) has no `CPlayerVRMovementParams` object. `meta_types.py` finds no file in SS4 that serializes `CPlayerVRMovementParams`, `CPlayerVRLegMovementAnimations`, `CHandAdjustmentScheme` or `CSkeletonAdjusterScheme` (SM: section 8) |
| `CPlayerVRMovementParams` members | rec 0x141843810..: `pvmp_fHeadAdjustmentRadius` +0x60 … `pvmp_psasHeadSkeletonAdjusterScheme` +0x80, `pvmp_phasHandAdjustmentScheme` +0x88, `pvmp_pvlmaLegMovementAnimations` +0x90, `pvmp_idWeaponHoldAnim_Right/Left` +0xa0/+0xa8 | dead (no content) |
| `CPlayerVRLegMovementAnimations` (`PlayerVRLegMovementAnim`, `…ForCrouch`, `PlayerVRLegMovementPose`, strings 0x141843e08 / 0x141843d18) | consumed only by the movement handler (0x140ac7710, array params+0x90, 0x30 per pose) | **dead** |
| **`CIKStaticModelEntity`** (class name 0x14198efa8; `en_ctIKIterations` +0x420, `en_ppvmpPlayerVRMovementParams` +0x428) | script `InitPlayerVRMovementHandler` **0x14061e790** (SM 0x14061e610) builds the same movement handler on a static model from `+0x428` (0x140ac70c0). `SetHandAdjustmentHandler` 0x14061e600, `SetIKTarget` 0x14061e270 (native IK solver 0x140d00ac0 / 0x140d021b0 at +0x430), `SetIKStrength` 0x14061e5a0 (+0x430 → +0x38). Data refs only through the script table 0x14198edb8/0x14198edf8. | **dead in content**: no `.ep/.rsc/.wld/.lua` in SS4 references `CIKStaticModelEntity`, `InitPlayerVRMovementHandler` or `SetHandAdjustmentHandler` (small and large file scans) |
| **VR head + hand adjusters on the 3rd-person body** | **0x14047e6a0**(player) (SM 0x14047ecf0, the 12th call of player init in both builds), called at 0x14042a50d right after vrd creation. From the model's `CSamCustomModelData` (renderable+0xd0 → +0x18 config → `0x140c10c20` = +0xa0): `scmd_phasHandAdjustmentScheme` +0xd8 (rec 0x141685d08) → `CHandAdjustmentHandler` (0x30, vtable 0x14169cdc8, init 0x140237c00) at **player+0x2470** (`en_phahHandAdjuster`, rec 0x141865a68; SM +0x2478). `scmd_psasHeadAdjustmentScheme` +0xe0 → `CSkeletonAdjuster` (0xb0, vtable 0x141bea740, init 0x14094a370) at **player+0x2478** (`en_psaHeadAdjuster`; SM +0x2480). Both are registered with 0x140bf6b20. Driven every player step (0x140486a40, at 0x140488740..0x1404887cd): hands ← `0x1402378a0`(h, R = 0x14043c560, L = 0x14043c540), head ← `0x140946a40`(sa, head world 0x14043cb50). The world→model conversion is in 0x140946a40 (renderable placement +0x3c quat / +0x4c pos, stretch `vt+0xf0`). | **live code, dead content**: Sam's `SeriousSamSchemes.rsc` (used by every player `.mdl` checked: SeriousSam, BlackTop, GroovyGregory) has a DTTY table with `Ptr<CHandAdjustmentScheme>` / `Ptr<CSkeletonAdjusterScheme>` but no struct for either and no external RFIL for them, so both pointers are NULL. (The method was checked: `CArmsDescriptionScheme`, which *is* embedded, does appear.) +0x2470/+0x2478 therefore stay NULL. **Dormant-callable** only with mod-made scheme resources (not recommended) |
| **Remote bodies with replicated poses** | Same path. 0x14043b920 returns the replicated vrd+0x348/+0x364 when vrd+0x328 == 0 (no device access). 0x14043d750 returns the replicated vrd+0x32c the same way (`lab/sam4_devcallers_decomp.c`). So *if* the schemes existed, remote bodies would follow replicated hands/head. | design only (dead content) |
| **Live arm IK: `CHandsAdjuster`** (not VR) | `prj_bPlayerHandsAdjuster` (0x1422472d8, `.data` initial value 1). Created in 0x14047e490 (0x48 bytes, vtable 0x14169d4a0, init 0x140239540, from `scmd_padsArmsDescriptionScheme` +0x100 via 0x1401fa700) at **player+0x26a8**. Stepped from the player step (0x140239d60). With a right-hand weapon it moves the body's left hand to the weapon attachment `LeftHandGripEffector` (IDENT 0x142280af0), weighted by the anim value `IK.Power` (0x142280b00). Apply `vt+0x20` = 0x1402394a0. | **live on every player.** It will fight the mod's arm IK unless the mod's handler runs after it (section 5) |
| Other live handlers on legged puppets | `en_pmhMotionHandler` +0xca8 (`CLeggedPuppetMotionHandler`, rec 0x1416c8708), `en_pmhCharacterLocomotionHandler` +0xcb8, `en_psaSpineAdjuster` +0xcb0; feet IK `CFeetAdjustmentHandler` (cvars `prj_ctFeetAdjustmentIKIterations` 0x142247e1c, `prj_fMaxFeetAdjustmentDistance` 0x142247e18, readers 0x140233580 / 0x1402352e0; scheme `scmd_pfasFootAdjustmentScheme` +0xd0, no SS4 file serializes `CFootAdjustmentScheme`) | locomotion: live; feet IK: content-dead for Sam (inferred, same scan) |
| **VR controller / hand models** | Two per-hand objects in vrd (+0x3d8 right, +0x4e0 left; size 0x108), set up in vrd init 0x14043e330 via 0x140721b20. Ticked by 0x1407228c0 from 0x1404399c0 (both hands), posed in the render view (0x1407234d0 at 0x14048f4d9/0x14048f508), drawn by 0x140722e50 (`vr_bForceControllerVisible` 0x1422aace4). Models come from `GlobalVRParameters.rsc` (`CGlobalVRParams`, `ggp_gvrpGlobalVRParams` +0xd98): Oculus L/R (+Ghost) and ViveWand (+Ghost) under `Content/Shared/Models/Objects/Controllers/`. A hand model `gvrp_pmcHandModel` +0xa8 (rec 0x14164ab68) gets per-finger animations `Index_Pose`, `Middle_Pose`, `Ring_Pose`, … (IDENTs 0x14228cd88..cda8). Their strength is device slot 0x1e0 per finger × `vr_fSkeletonFingerPoseFactor` (0x1422493a0, sole reader 0x140722c5f), gated on device slot 0x1d8 (hand skeleton available). | controllers: **live** once vrd exists. Hand model and finger poses: **dead in content** (no hand `.mdl` in the `GlobalVRParameters.rsc` RFIL; no content file contains `Index_Pose`/`Middle_Pose`/`Ring_Pose`) |
| `*_VR.mdl`, Knuckles `.ep` | VR weapon models: `Minigun_VR.mdl`, `GrenadeLauncher_HD_VR.mdl`, `Detonator_VR.mdl`, `Cube_FP_VR.mdl` (referenced from the weapon `.ep` files, chosen by `en_bOwnerPlayingInVR`, R3 s2). `CubeWeaponRenderingVR[_Knuckles\|_Oculus].ep` are `CObjectHolderWeaponRenderingParams` (per-controller held-object offsets), used by `Items/Misc/Torch.ep`. **None of them references an arms/hand skeleton or mesh** (IDENT and RFIL tables checked; only `Detonator_VR.mdl` has an `Activate_OnlyRightHand` animation). | weapons: live with vrd (R3) |
| `vr_bUseThirdPersonFallbackView` (0x1422486dc) | comfort third-person camera (readers 0x140438d60, 0x14043ef60, 0x14043f974, 0x140440ab4, 0x140509cfd) | not a body feature; it shows the unchanged 3rd-person body |

Fusion 2017 VR (`Bin/x64/Sam2017_VR.jotn-vr-stock`) has the same classes, plus a script
`CPlayerActorPuppetEntity::SetFirstPersonAvatarMode` that SS4 lacks. Its content has only
`PlayerVRParameters.rsc` and no movement-params or adjuster files either. The VR body content was never
shipped in this engine branch.

## 2. Player model, skeleton and bones

### Getting from the player to the skeleton
| step | address / offset |
|---|---|
| renderable | `player+0x1e0` (`vt+0xc8` = 0x1402a7b00). Its world placement is renderable+0x3c (quat xyzw) / +0x4c (pos), stretch via `vt+0xf0` (0x140946a40, 0x140bf8c10). Owner entity at renderable+0x58 (0x140b86120) |
| model instance | renderable+0xd0 (`0x14004e5c0`). Tag list at mi+0x50 (0x140c10e60), state vars via 0x140c11210 |
| model configuration | mi+0x18 (`0x140c111c0`). Custom model data (`CSamCustomModelData`) at config+0xa0 (`0x140c10c20`) |
| anim queue | `0x140c10420`(mi, create) = mi+0x10 (created lazily). Motion-handler list in its implementation object (`0x140bac1c0` add, `0x140bad190` remove, `0x140bad330` enumerate) |
| skeleton | config+0x68 (`0x140c10df0`) → LOD0 `0x140c288d0` (skeleton+0x68 when count +0x60 > 0) |
| bone by name | **0x140c282a0**(lod, IDENT) (SM 0x140c28940): binary search in the {IDENT, index} map at lod+0x28 (count +0x20). Bones are at lod+0x10, **0xf0 bytes each**. Root index lod+0 (0x140c28920). In a `SkeletonBone`: +0x08 = parent IDENT (script `GetParentBone` 0x140075190), +0x48 quat / +0x58 vec3 = rest pose **local** (copied into the pose arrays as the default, 0x140c11f90), +0x64 = rest model-space placement (used for `BSS_MODEL`, 0x140077000). The other fields are unknown |
| string → IDENT | `0x1410c65f0`(&out, "Name") (SM 0x1410c6da0). IDENTs are interned pointers and are compared by pointer everywhere above |
| bone model-space matrix | **0x140c2bb90**(time, mi, IDENT, Matrix34f* out, 0, 0) (SM 0x140c2c180). It forces bone evaluation (lazy, see below). The script wrappers compose it with the renderable placement: `GetBonePlacementAbs` 0x140075270 → 0x1400785d0 (world), `Obj` 0x140075300 → 0x140078b10, `Rel` 0x140075390 → 0x140078e30. `IfModelHasBone` 0x140074f70, `IfEntityHasBone` 0x1400750a0, `GetAllBones` 0x1400757e0, `CStaticModelEntity::GetDefaultBonePosRot` 0x14070df30 |

SM script VAs (harvest `SamSM.json`): `GetBonePlacementAbs` 0x1400755b0, `GetParentBone` 0x1400754d0,
`CreateScriptedBonePathFollower` 0x1400758c0, `CScriptedBonePathFollower::SetBonePlacement` 0x140074cf0.

### Player skeleton (content)
Every player model checked (`Models/Characters/SeriousSam/SeriousSam.mdl`, `SeriousSam_BlackTop.mdl`,
`Characters_Multiplayer/DancingDenzel/GroovyGregory.mdl`) uses
`Content/SeriousSam4/Models/Characters/SeriousSam/FBX/Skeletons/SeriousSam.skl`, animations `SeriousSam.anh`,
attachments `SeriousSamAttachmetns.rsc`, schemes `SeriousSamSchemes.rsc`. Bone IDENTs in the `.skl` (65,
Mixamo-style naming):

`Hips, Spine, Spine1, Spine2, Spine3, Neck, Head, Jaw, LeftEye, RightEye, ViewMover,
LeftShoulder, LeftArm, LeftArmTwist1, LeftForeArm, LeftForeArmTwist1, LeftHand, LeftWeapon,
LeftHand{Index,Middle,Ring,Pinky,Thumb}{1,2,3}, (same for Right…),
LeftUpLeg, LeftUpLegTwist1, LeftLeg, LeftFoot, LeftToeBase, (same for Right…)`.

The parent links are not readable from the IDENT table. Read them at runtime with `SkeletonBone+0x08`. The
`*_IK_EndEffector` and `LeftHandGripEffector` names are *attachments* (`SeriousSamAttachmetns.rsc`), not
bones. `ViewMover` is the first-person view bone (`psd_idFirstPersonViewMoverBone`, rec 0x1416d6400; used by
0x1404795a0 in avatar mode). `LeftWeapon`/`RightWeapon` are the weapon bones under the hands. The
first-person arms (`Models/Weapons/Arms/Arms.mdl`, `Arms.skl`: `Root, ViewMover, L_UpperArm, L_LowerArm, L_Hand,
…, L_Weapon, R_…`) are a separate model (`en_pmcFirstPersonArmsModel` +0xff0, rec 0x1418623a8; SM +0xff8).

### Where bone transforms become final
* Lazy update: **0x140c29660** (SM 0x140c29c50) → 0x140c17180 → 0x140c12cf0 → **0x140c11f90** (SM 0x140c12710,
  128-byte sig). The bone-matrix query 0x140c2bb90 reaches the same 0x140c12cf0 through 0x140c17630 (call at
  0x140c179d6). It is called from render (0x1401df3e0) and from gameplay queries (weapon attachment
  0x1402d60e0, barrel 0x1402d7360, bone queries above). 0x140c11f90 fills the pose arrays from the rest bones,
  calls **0x140bad5b0** (SM 0x140badcc0), and copies the result back into the render-model bone entries
  (array at +0x50, 0x38 each: +0x08 quat, +0x18 pos, +0x28 → `SkeletonBone`, +0x30 lock flag).
* 0x140bad5b0 blends the active animations (0x140ba9830). **Then** it calls every registered motion
  handler, in registration order: `handler->vt[0x20](handler, ctx, pose)`.
* **Which thread runs the evaluation is unknown.** Render and simulation both trigger it.

## 3. Hook point and bone data layout

### The pose a motion handler receives (`vt+0x20`, arg 3), from 0x140bad5b0 / 0x140c25de0 / 0x1409496c0
| offset | content |
|---|---|
| +0x00 / +0x08 | `IDENT*` per bone / bone count (int) |
| +0x10 / +0x18 | position records, **0x28** each / count: +0x00 int flag (handlers set 1), +0x04 vec3 rest, +0x10 vec3 animation, **+0x1c vec3 result** |
| +0x20 / +0x28 | rotation records, **0x34** each / count: +0x00 int flag, +0x04 quat rest, +0x14 quat animation, **+0x24 quat result (x,y,z,w)** |
| +0x30 / +0x38, +0x40 / +0x48 | 0x10-byte records (floats at +0x04 / +0x0c; dispatched to `vt+0x38` / `vt+0x40`). Meaning unknown (stretch/morph?) |
| +0x50 / +0x58, +0x60 / +0x68 | when the +0x58 count equals the bone count, positions go to `vt+0x48` instead of `vt+0x28`. 0x58-byte records at +0x60. Meaning unknown |

The values are **parent-relative (local) QuatVect**. Evidence: they are initialized from the skeleton's local
rest pose (`SkeletonBone+0x48/+0x58`, 0x140c11f90), the animation blend lerps/slerps them per bone
(0x140bad5b0), and `CBonePathFollower` converts `BSS_MODEL`/`BSS_WORLD` targets to them by walking parents
(0x140077000). The skeleton adjuster changes the chain root by pre-multiplying its local value (0x1409496c0).

### CMotionHandler ABI (base vtable 0x141e4c138, SM 0x141e309b8; ctor 0x140c25cb0 / SM 0x140c263e0; dtor 0x140c25cd0 / SM 0x140c26400)
* Object: +0x00 vtable, +0x08 pointer to the list of anim queues it is registered on, +0x10 count, +0x14
  capacity. The list is engine-allocated (grown by 0x140060b90 in 0x140bac1c0). The base dtor unregisters from
  every queue (0x140bad190) and frees the list.
* Slots: [0] scalar-deleting dtor; [1] type descriptor (callers read +0x10 as the class name, 0x140ac70c0);
  [2], [3] void (0x140044530 no-op in the base; `CBonePathFollower` uses [2] = 0x140077000 to prepare its
  per-frame targets, caller not pinned); **[4] `+0x20` apply(h, ctx, pose) → BOOL changed** (base
  0x140c25de0 calls per-bone [5] `+0x28` pos, [6] `+0x30` rot, [7] `+0x38`, [8] `+0x40`, [9] `+0x48` pos-alt).
* Registration: **0x140bf6b20**(renderable, h, 0) → `0x140bac1c0`(queue, h). It is a no-op if already present,
  appends to the end, and adds the queue to h+0x08. Removal: **0x140bf7090**(renderable, h) → 0x140bad190.
  When a queue or model instance dies, its destructor **0x140ca7470** only unlinks itself from each handler
  (h+0x10 drops). It never deletes handlers, so a handler the mod owns stays valid. When its count reaches 0, it
  must be re-registered on the new model instance (player model change, respawn).
* Handlers run in registration order. Natively registered on a player: locomotion (+0xca8), `CHandsAdjuster`
  (+0x26a8, created lazily and re-created by 0x14047e490 from five sites: 0x1404330ff, 0x140433219, 0x140480e5a,
  0x140488ab5, 0x14052e481), and the dead VR adjusters.

### Existing engine override handler: `CBonePathFollower`
* Class rec 0x1415940d8: `bpf_abpfiBones` +0x18 (0x40 per bone), `bpf_vStretch` +0x28, `bpf_hpslSkeleton` +0x34,
  `bpf_preTarget` +0x38. Per bone (`CBonePathFollowInfo`): `bpfi_idBone` +0x00, `bpfi_iBPFParent` +0x08,
  `bpfi_fLength` +0x0c, **`bpfi_qvDesired` +0x10**, **`bpfi_Flags` +0x2c** (`BSF_SET_POSITION` 1,
  `BSF_SET_ROTATION` 2), **`bpfi_bssDesiredSpace` +0x30** (`BSS_PARENT` 0, `BSS_MODEL` 1, `BSS_WORLD` 2,
  enum records 0x141593b60..b90), **`bpfi_fStrength` +0x34**, `bpfi_iHierarchy` +0x38.
* Native creation (as the script `CreateScriptedBonePathFollower` 0x140075580 does): alloc 0x40, ctor
  **0x1400766c0**(mem, `CStaticStackArray<IDENT>*` bones) (SM 0x1400769c0), `0x140bf6b20`(renderable, bpf, 0),
  init **0x140079680**(bpf, renderable) (SM 0x140079960, 24-byte sig). Vtable 0x141596858: [2] 0x140077000
  prepare (space conversion), [6] 0x140077f20 rotation slerp by strength, [9] 0x140077e20 position lerp
  (÷ `bpf_vStretch`), [4] = base apply. SM [2]/[6]/[9] = 0x140077300 / 0x140078230 / 0x140078130.
* Caveat: position overrides go through slot [9] only when the pose's +0x58 count equals the bone count
  (0x140c25de0). Whether that holds for the player's model is **unknown**, so a hips translation may not
  apply.
* Other mechanisms checked and rejected for full-body use: `LookAt*` scripts (0x14045e520..790, head/eyes
  only), `CEyeMovementHandler` (+0xa50), FABRIK script solver (`CreateFABRIKSolver` 0x140e25790, a math helper
  with no pose output), the native IK chain solver used by the adjusters (0x140d00ac0 ctor, 0x140d021b0 init
  chain, 0x140d00c90 solve). That solver is usable but needs the schemes above.

## 4. First-person body

* **Normal first person:** render view **0x14048dad0** (SM 0x14048e0e0). If not third person (`vt+0xdf0` =
  0x1402be4d0) and `0x14047edb0` is false, it tests `en_pnfNetFlags & 0x1000` (+0x7a4, rec 0x1416ec528). Clear →
  **0x14048ddd4: `0x140ca3280(player)`** (SM 0x14048e3e4). That stores the *view owner*
  global 0x1422d7790 (SM 0x140ca39c0 / 0x1422b4940). The renderer skips renderables whose owner equals it
  unless renderable+0x60 bit 27 is set (0x140b86120, 0x140fcc6a0). It is reset with 0 at 0x14048f614.
  **So the player's own body is not drawn in first person, and (inferred) not in mirrors rendered inside the
  same view.** Only the arms model (+0xff0) and weapons are drawn. There is no `plr_/gam_/hud_` cvar for
  body/legs. `hud_bShowWeapon` 0x142247284 is the only weapon-visibility cvar. `prj_iGameOptionThirdPersonCamera`
  0x142247214 (readers 0x140478230, 0x14049a092) and script `Set3rdPersonView` 0x140463b70 (writes
  `en_bAvatarViewMode` +0x788 = "third person", replicated) switch to the full third-person camera.
* **First-person avatar mode** (`PNF_AVATAR_SEEN_IN_1ST_PERSON` = 0x1000, `PNF_1ST_PERSON_AVATAR_WITH_WEAPON` =
  0x80000, `PNF_1ST_PERSON_AVATAR_INTERRUPTIBLE` = 0x1000000; enum table at 0x1416cfcd0..): set by the player
  state `CPSFirstPersonAvatarAnimation` through script `PlayFirstPersonAvatarAnim` 0x14045e940 (SM 0x140460540;
  content: `01_Forum_Script_ChapterControl.lua`, `02_Pompeii_PlaceBeacon.lua`, `12_Tunguska_Brand_Control.lua`).
  In 0x14048dad0: view owner 0 (0x14048dd25), model tag **`FirstPersonAvatar`** added to the body
  (`0x140c10e60`(mi, IDENT 0x142286b08) at 0x14048dd43; SM 0x140c115e0, IDENT 0x142263c88), state var `HideBody`
  = 1 (0x14048dd6a). With 0x80000 also `HideArms` = 1, and the weapon's first-person arms model hidden
  (`weapon vt+0x9c0` → `0x140c108c0`, bit 0 at +8). All of it is undone after the frame (0x140c11260 at
  0x14048f635, 0x140c11a00 at 0x14048f65c). `FirstPersonAvatar` is a material condition in Sam's `Head_Mat`,
  `Top_Mat`, `Sleeves` (and other characters' head/hair/eyes/top materials). **Inferred:** in avatar mode
  head and shirt are hidden while arms skin and legs stay visible. No content file references `HideBody` or
  `HideArms`. **The flag cannot be reused as is.** It also replaces the view with the `ViewMover` bone
  (0x1404795a0 → 0x140bf8c10), skips the weapons' `vt+0x630` call (0x14047e850), resets the look direction
  (0x140488402 in the player step: 0x14049def0 on player+0x22c0), and it replicates (`en_pnfNetFlags` flags 0x4000).
* **VR hands/weapons in first person with vrd:** weapons are placed at the controller pose (0x1402d8ad0, R3 s2;
  VR model configs via 0x1402d7f90 / 0x1402dd0a0). The controller models (section 1) are drawn when a hand is
  empty or forced. Whether the first-person arms model is drawn with VR weapons is **unknown** (not traced).

## 5. Recommended implementation

### A. Body pose on every machine: one mod-owned motion handler per VR player
1. **Object:** construct with the engine base ctor (0x140c25cb0 / SM 0x140c263e0) into mod memory of at least
   0x18 bytes plus mod fields. Then point +0 at a mod copy of the base vtable (0x141e4c138 / SM 0x141e309b8, 10
   slots) with **[4] = mod apply** and [0] = a mod dtor that calls the base dtor 0x140c25cd0 / SM 0x140c26400
   (this unregisters it and frees the engine list) before freeing the mod memory. Keep [1] (base type
   descriptor) so engine logging and type checks see a valid `CMotionHandler`.
2. **Register** on the sim thread: `0x140bf6b20(player+0x1e0, h, 0)` (SM 0x140bf7420), once the renderable
   exists. Each tick, re-register if `h+0x10 == 0` (model instance replaced) **or** if `player+0x26a8`
   (`CHandsAdjuster`) changed since the last tick: remove with 0x140bf7090 and add again, so the mod's handler
   is last in the list. Unregister on player destruction/VR off. Players: every player with vrd (local and
   remote), on clients, the host and desktop peers. On a dedicated server, leave it off unless hit detection
   should follow IK. Bone queries (0x140c2bb90) also run the handlers.
3. **Inputs (sim thread, per VR player):**
   * local: head world 0x14043cb50, controllers 0x14043b920(vrd, out, 0 = right / 1 = left).
   * remote: the same calls return the replicated values without touching the device (vrd+0x328 == 0
     branches). Alternatively read `en_qvNetRightVRControllerPlacement` +0x25d0, `…Left…` +0x25f4 (world), and
     `en_qvNetTrackerPlacement` +0x2618 (device-local). SM: +0x25d8 / +0x25fc / +0x2620.
   * **Risk:** the remote head world is the *receiver's* anchor (vrd+0x2c0, rebuilt from the body in
     0x14043f180, with turn yaw vrd+0x674 local state) ∘ the replicated device-local head. It may disagree with
     the sender. The mod channel (PLAN MP-1) should carry a world-space head, or the anchor, to remove the
     doubt. Runtime check: compare the remote head with the remote weapon (hands are world space and exact).
   * Convert world → model space with the renderable placement (+0x3c quat, +0x4c pos) and stretch
     (`vt+0xf0`), as 0x140946a40 does. Run `core::body_ik` in model space. Hand targets: the replicated
     controller pose is where the weapon is drawn (0x1402d8ad0), so place the hand so that its
     `RightWeapon`/`LeftWeapon` child bone lands on it. Use the rest local offset `SkeletonBone+0x48/+0x58`.
   * Publish the result (pelvis QuatVect plus joint positions / torso yaw) in a per-player double-buffered
     snapshot with a sequence number. The apply runs at an unknown time and thread.
4. **Apply (`vt+0x20`)** never mutates game state. It reads the newest snapshot. On first use per model it
   resolves pose indices by comparing `pose+0x00` IDENT pointers with IDENTs interned once via 0x1410c65f0 (SM
   0x1410c6da0), and parents via `0x140c282a0` + `SkeletonBone+0x08`. It runs FK over the incoming local
   arrays, then writes local results for Hips (+0x1c pos and +0x24 quat), Spine..Spine3/Neck/Head (rotation
   only, distributed), Left/RightShoulder/Arm/ForeArm/Hand (swing toward the solver joints; twist bones get a
   share of the forearm twist), and Up-leg/Leg/Foot. It sets each record's flag (+0x00) to 1 and returns 1.
   If the snapshot is older than 0.5 s, it fades its weight to 0 (PLAN), which leaves the native animation.
5. **Alternative with less mod math:** `CBonePathFollower` created natively (0x1400766c0 → 0x140bf6b20 →
   0x140079680). Write `bpfi_qvDesired`/`bpfi_bssDesiredSpace = BSS_MODEL`/`bpfi_Flags = 3`/`bpfi_fStrength`
   per bone each tick from the sim thread. It is engine-tested, but the position channel caveat (section 3)
   and the unpinned prepare caller make it the second choice. Use it if the mod handler cannot be made
   stable.

### B. First-person body (local VR player only)
* Hook the view-owner setter **0x140ca3280** (SM 0x140ca39c0; leaf `mov [rip+g], rcx; ret`). Alternatively
  replace the call at 0x14048ddd4 (SM 0x14048e3e4) with a call to a mod thunk. When the argument is the
  local VR player and the body option is on, store 0 instead.
* For that frame, mimic avatar mode without its flag: `0x140c10e60(mi, *(IDENT*)0x142286b08)` (SM 0x140c115e0 /
  0x142263c88) before drawing, `0x140c11260` after (SM 0x140c119e0). Also hide the weapons' first-person arms
  models (`weapon vt+0x9c0` → 0x140c108c0 / 0x140c11a00) so only the body's hands remain. Runtime check: which
  surfaces the `FirstPersonAvatar` tag hides. If the torso disappears too, drop the tag and collapse the
  `Head` bone (scale or move into `Neck`) in the motion handler for the local view only.
* Mirrors: with the view owner cleared, the body should also appear in mirrors (inferred), but the tag also
  hides the head there. Runtime check, then decide whether the tag is applied only to the main eye passes.
* Do not set `PNF_AVATAR_SEEN_IN_1ST_PERSON`: it replicates and rewires the view, look and weapons (section 4).

### C. Guards (desktop peers with a NULL device global, and the server)
1. **New, required before any remote VR player can exist on a desktop peer:** patch **0x14043aac0** (SM
   0x14043ac40; `mov rcx,[g_device]; mov rax,[rcx]; jmp [rax+0x1d8]`) into a null-checking thunk that returns
   0, the same way as the slot 0x208 sites. Callers: 0x1407228ee, 0x140722b92, 0x140722cc3 (controller objects,
   reached every tick from 0x1404399c0 for every alive player with vrd: gate `vt+0xa28` = 0x14047fb50 and
   `PNF_ALIVE`, no local check), 0x14047c2d1, 0x14047c699, 0x14046ea2f. The finger loop behind it also uses
   slot 0x1e0, which is reached only when 0x1d8 returns non-zero.
2. The mod handler and the IK inputs must use only the vrd+0x328 == 0 (remote) branches on peers without a
   device. 0x14043b920, 0x14043d750 and 0x14043cb50 null-check the device on their local paths. Never call
   device slots from the handler.
3. Keep the R3 s10 guards (vrd_init slot 0x208, controller-world NULL vrd, ClientActionVR without vrd).
4. Do not enable the native VR adjusters (0x14047e6a0) by supplying schemes. They would overwrite the same
   bones. If they ever appear (+0x2470/+0x2478 ≠ 0), unregister them with 0x140bf7090.
5. Fallback: no vrd, or a pose older than 0.5 s → the handler returns 0 (native puppet).

## 6. Runtime checks (headset/lab, in order)
1. Log the player's pose IDENT list and count on registration. Check the 65 bones and the parent links, and
   which of +0x30/+0x40/+0x50 counts equal the bone count.
2. Write a fixed local rotation to `LeftForeArm` from the handler. It must be visible locally (third-person
   camera) and on a second machine. Measure which thread calls the apply.
3. Remote head vs. remote weapons (anchor consistency, 5.A.3).
4. First-person body: view-owner hook plus `FirstPersonAvatar` tag. Check what is hidden, and check mirrors.
5. Desktop peer with a remote VR player: no crash with the 0x14043aac0 guard. Without it, expect a fault at
   0x14043aaca (do not test without the guard).

## 7. Status table (Gate R9)

| item | status |
|---|---|
| VR movement handler / leg animations / `CPlayerVRMovementParams` | dead in content (code live) |
| `CIKStaticModelEntity::InitPlayerVRMovementHandler` and friends | dead in content |
| VR head/hand adjusters on the 3rd-person body (0x14047e6a0) | live code, dead content (NULL schemes) |
| replicated hand/head → remote body | not native (adjusters dead). The mod must add it |
| `CHandsAdjuster` (left hand to weapon grip) | live, all players, default on |
| VR controller models | live with vrd. Hand model and finger poses dead in content |
| first-person own body | not drawn natively. Avatar-mode path dormant-callable via tag (section 4) |
| motion-handler registration (0x140bf6b20) / `CBonePathFollower` | live engine mechanisms, recommended hooks |
| slot 0x1d8 null deref 0x14043aac0 on desktop peers | **risk, unguarded** |

## 8. SM port notes
* Ported uniquely (64-byte signatures unless noted): hand set 0x140237620, head set 0x140946fd0, head solve
  0x140947240, register 0x140bf7420, unregister 0x140bf7990 → 0x140bad8b0, queue add 0x140bac8e0, dispatch
  0x140badcc0, base apply 0x140c26500, bones update 0x140c29c50, bone matrix 0x140c2c180, bone lookup
  0x140c28940, BPF ctor 0x1400769c0, render view 0x14048e0e0, tag remove 0x140c119e0, VR movement handler init
  0x140ac7580 / update 0x140ac7bd0, vrd buttons 0x140439b40, view placement 0x140479c00, head world 0x14043cc90,
  local head 0x14043d890, hand-adjust apply 0x140237870, hand-adjust init 0x140237980 (24), skeleton adjuster
  init/apply 0x14094a8b0 / 0x140949c00, hands adjuster apply 0x140239210, net copy/write-back 0x14048ad90 /
  0x14048aa30 (24), vrd init 0x14043e480, player init 0x14042a430 (24).
* By call order or vtable: VR adjusters 0x14047ecf0 (12th call of player init in both builds), vrd tick
  0x14043f2d0 (4th call of player `vt+0x210` 0x14048a700), controller tick 0x140722020 (1st call of
  0x140439b40), controller world 0x14043ba20 (matches `manual.json`), slot 0x1d8 thunk 0x14043ac40 (1st call of
  0x140722020; same three instructions, device global 0x142287ed0). SM player vtables 0x141831500 / 0x141852a50:
  `vt+0x208` = 0x140486fd0, `vt+0x210` = 0x14048a700, `vt+0xc8` = 0x1402a78c0.
* SM member offsets (meta records): `en_phahHandAdjuster` +0x2478, `en_psaHeadAdjuster` +0x2480,
  `en_qvNet{Right,Left}VRControllerPlacement` +0x25d8 / +0x25fc, `en_qvNetTrackerPlacement` +0x2620,
  `en_pmcFirstPersonArmsModel` +0xff8. `en_pnfNetFlags` +0x7a4, `scmd_*` +0xd8/+0xe0, `pvp_ppvmpPlayerVRMovementParams`
  +0xd0, `bpfi_*` and `bpf_abpfiBones` are unchanged. `CHandsAdjuster` slot in SM (Sam4 +0x26a8): **unknown**.
  Not ported: 0x14047e490 (hands adjuster creation), 0x140464ca0 (state var).
* SM content: SM uses SS4's `PlayerVRParameters.rsc`, `GlobalVRParameters.rsc` and `PlayerPuppet.ep` from its own
  `00_All.gro`, plus `SeriousSamSchemes.rsc` and `SeriousSamSchemes_SM.rsc`. The `meta_types.py` scan of SM's
  `00_All.gro` and `50_628492_00_All.gro` finds **no** file that serializes `CHandAdjustmentScheme`,
  `CSkeletonAdjusterScheme`, `CPlayerVRMovementParams`, `CPlayerVRLegMovementAnimations`, `CIKStaticModelEntity` or
  `CIKStaticModelProperties`. SM's `SeriousSamSchemes.rsc` and `SeriousSamSchemes_SM.rsc` embed only
  `CArmsDescriptionScheme` of these. So section 1's classification holds for SM.
