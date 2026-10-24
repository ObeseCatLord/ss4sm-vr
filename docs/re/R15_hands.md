# Gate R15: separate VR hands, finger tracking and two-hand grips

Static analysis only. Sources: the Ghidra copy `lab/ghidra_r15/sam4`, capstone, the harvest JSONs and content
extracted with `tools/gro_extract.py` into `lab/r15/content` and `lab/r15/sm_content`. Nothing was run: no game,
Wine, Proton, Monado or XR runtime. Addresses are Sam4.exe 616154 PE VAs unless marked **SM** (SamSM.exe 628492).
Each SM address says how it was ported (section 10). Decompiles are in `lab/r15/b1.c`..`b4.c` and the R9 dumps in
`lab/r9/*.c`; none of them is committed. Member offsets come from reflection records (`tools/meta_class.py`,
`tools/meta_members.py`, record address given). Unreflected fields are marked "(unreflected)". Anything not backed
by an address is marked **unknown** or **inferred**.

New read-only tools:
* `tools/meta_enum.py` dumps a reflection enum table from one enumerator name (for example WeaponFlags,
  TwoHandedVRWeaponOrientation, GloveFingers).
* `tools/sig_at.py` checks a port found by vtable slot, call order or constant shift. It compares the source's
  wildcarded signature with the bytes at the candidate address.

Design reference: `~/Documents/eduke32_vr` (GPL-2), described here as behaviour only (section 7). No code was copied.

## 0. Summary

1. **The first-person arms are the weapon's own model.**
   * Each weapon's first-person model is an `X_Arms.mdl`, for example `Pistol_Arms.mdl`. Its root uses the arms
     skeleton `Arms.skl` (42 bones: `Root, ViewMover`, then per side `UpperArm, LowerArm, LowerArm_Twist, Hand`,
     `Thumb/Index/Middle/Ring/Pinky_01..03`, `Weapon`). Its anim host holds the per-weapon arm+gun animations.
   * It has two children. The gun mesh hangs on `R_Weapon` (`L_Weapon` in the `_Arms_L` dual-wield variant). The
     arms mesh is a child named **`Arms`**.
   * The weapon function **0x1402e2910** (static slot +0xc28; slot map caveat in 1.2) puts the character's arms skin
     there, from player `+0xff0` `en_pmcFirstPersonArmsModel`.
   * In VR, the weapon placement 0x1402d8ad0 (R3) moves this **whole root** to the controller. Both arms keep
     their flat-screen pose around the gun. That is why the lab shows engine arms "floating" with the weapon.
2. **There is no per-bone scale and no per-side surface.**
   * `Arms.mdl` is one mesh (`Arms.bmf`) with one material, and that material has no hiding condition.
   * The pose a motion handler gets carries only a quaternion and a position per bone (R9 s3). The values copied back
     into the render bones are those two (0x140c11f90).
   * So one arm of an instance cannot be hidden by tags, surfaces or scale. It can only be posed: driven to a
     controller, or **parked behind the eyes**.
3. **The engine has a native per-hand "hand model" path, but no content for it.**
   * `CVRController` (vrd `+0x3d8` right, `+0x4e0` left) has `vrc_pmiHandModel` `+0xe0`. It is filled from
     `gvrp_pmcHandModel` (`CGlobalVRParams +0xa8`, NULL in both games).
   * When that slot is filled, the engine places the model at the controller times a per-controller-type offset
     (`vrcm_qvHandOffsetPlacement`), and mirrors it for the left hand.
   * It drives five finger animations (`Thumb/Index/Middle/Ring/Pinky_Pose` over `Idle_Knuckles`) from device slot
     0x1e0, gated by device slot 0x1d8.
   * It draws the hand **instead of** the controller.
   * `Arms.mdl` has none of those animations, so only the slot, the placement and the draw are reusable.
4. **Native two-hand grip** (weapon step **0x1402de9a0**):
   * It applies only when weapon params `wp_bTwoHanded` (`+0x1b4`) is set.
   * The **off hand's fire bit** in player `+0x2338` must be held while the off controller is within
     `wp_fHandSnapMinDistance` (`+0x214`; if < 0, `pvp_fHandSnapMinDistance` `CPlayerVRParams +0xcc`) of the weapon's
     grip attachment. Then it sets `WF_GRIPPED_WITH_TWO_HANDS` (0x20) in `en_ulWeaponFlags` (`+0x168`) and starts a
     snap lerp (`pvp_fHandSnapLerpTime` `+0xc8`).
   * It releases when the bit is released. Distance does not matter for release.
   * The aim is a look-at from the main hand to the off hand, with a roll rule from `wp_eTwoHandedVROrentation`
     (`+0x1bc`), in **0x1402e2d70**.
   * The flag is not replicated. Each machine computes it from its own `+0x2338` and vrd poses.
5. **Controller models** are hidden with `vrc_bIsVisible` (`+0x94`) = 0. That is exactly what the scripts
   `HideRight/LeftVRController` do (0x1404385d0 / 0x140438610 → 0x140721af0). `vr_bForceControllerVisible`
   (0x1422aace4) must stay 0.
6. **Finger input.**
   * OpenXR `XR_EXT_hand_tracking` (already requested by `src/xr/runtime.cpp`) gives 26 joints with full orientation.
   * `XR_EXT_hand_tracking_data_source` can ask for controller-driven joints. Whether each runtime offers it is
     **unknown** and must be checked at runtime.
   * Fallback from the Index profile: index finger from `trigger/value` and `trigger/touch`; middle, ring and pinky
     from `squeeze/value`; thumb from the stick, trackpad and A/B touches.
   * Both sources should be reduced to per-joint curl angles, then applied to bones about flexion axes derived from the
     rest skeleton at runtime.
7. **Recommendation:**
   * Keep the engine's weapon arms instance as the hand that holds the gun.
   * Drive the rest of that instance with a mod motion handler: arm IK from a virtual shoulder; the off arm goes to the
     free controller with finger curls, or stays on the foregrip while gripped.
   * Add one mod-owned standalone `Arms` instance for hands with no weapon.
   * Feed the native grip with the squeeze instead of the fire bit.
   * Hide the controllers.
   * Others see the hands through the third-person body (`src/se4/body.cpp`) plus replicated finger curls. Section 8
     has the details.

## 1. First-person arms with a weapon

### 1.1 Content (SS4 `00_All.gro` / `50_616154_00_All.gro`; SM ships the same files, section 10)
| file | what it is |
|---|---|
| `Models/Weapons/Arms/Arms.mdl` | Arms mesh only. RFIL: `Resource/Arms.skl`, `Arms.bmf`, `Arms_AnimHost.anh`. One `CMeshInstance`, one material `Arms_0_Arm_Mat.mtr`, whose `CMaterialConditionHolder` has no condition entries. 26 per-character skins `Arms_<Name>.mdl` use the same skeleton. |
| `Arms.skl` | Bone IDENTs: `Root, ViewMover, L_UpperArm, R_UpperArm, L_LowerArm, L_Hand, L_LowerArm_Twist, L_Thumb_01, L_Index_01, L_Middle_01, L_Ring_01, L_Pinky_01, L_Weapon, L_Thumb_02/03, L_Index_02/03, L_Middle_02/03, L_Ring_02/03, L_Pinky_02/03`, the same for `R_`. 42 in total. Parent links are not in the IDENT table. Read them at runtime from `SkeletonBone+0x08` (R9 s2). |
| `Arms_AnimHost.anh` → `Arms_Animations.ans` | `Arms_IdleReady, Arms_Sprint, Arms_Idle, Walk, Arms_ActivateReady, TakeItem01` (empty-hands animations). Also `Arms_Typing.ans` (`Typing*`) and `Arms_Weapons.ans` (`*_Mount`). |
| `Weapons/Pistol/Pistol_Arms.mdl` (pattern for every weapon) | Skeleton `Arms.skl`, anims `Pistol/FBX/Pistol.anh`. Children: `Pistol` (gun mesh `Pistol.mdl`, on `R_Weapon`), `GrapplingHook` (on `L_LowerArm`) and **`Arms`** (`Arms.mdl`). |
| `Pistol_Arms_L.mdl` | Dual-wield left variant: gun on `L_Weapon`, `Arms` child, anims `Activate_SingleArm`, `Idle_SingleArm`, `FPVehicle_*`. |
| `Databases/Weapons/PistolWeapon.ep` | References both `Pistol_Arms.mdl` and `Pistol_Arms_L.mdl`, plus the `PlayerWeapon_Pistol_L/R.ctl` tools. |
| `Databases/Weapons/HandsWeapon.ep` | `CHandsWeaponParams`. Its first-person model is **`Arms.mdl` itself**: the empty-hands "weapon" `CHandsWeaponEntity` (melee and decapitation, held heads and tools, Sirian bracelet; members `en_htHandTool` +0x424, `en_bReady` +0x420, rec 0x141aab520..). When the player holds it is **unknown** (runtime check). |

The single-arm animation choice is native. In **0x1402e13d0** (0x1402d8aa0 calls it as
`0x1402e13d0(w, out, 'Walk', 'Walk_SingleArm')`; IDENTs 0x142282600 / 0x142282638), the `_SingleArm` name is used
when `wp_bUsesSingleArmAnimations` (`+0x1c4`, rec 0x14170ee28) is set and the owner is dual wielding (0x14047ef60 or
0x1402ad380). How the single-arm animations keep the unused arm out of view is **unknown** (the `.ans` tracks were not
decoded).

### 1.2 Engine structures (model instance, children, hiding)
| item | evidence |
|---|---|
| weapon model instance | `en_pmiWeapon` weapon+0x90 (rec 0x141710950). Owner handle `en_henOwner` +0x98. Params `en_pwpWeaponParams` +0xa8. |
| model instance fields | +0x10 anim queue (`0x140c10420(mi, create)`, R9). +0x18 configuration (`0x140c112d0(mi, cfg)` sets it and rebuilds, 0x140c0a740). +0x30 children container (array +0x18, count +0x20; walked by 0x140c10670). |
| child object | +0x00 IDENT name (compared by pointer in **0x140c10670**(mi, IDENT)). +0x08 flags, bit 0 = hidden (**0x140c108c0** sets it, **0x140c11a00** clears it, 0x140c10910 tests it). **+0x38 embedded model instance** (0x140c104c0 returns child+0x38. 0x1402e2910 calls `0x140c112d0(child+0x38, cfg)`). |
| recursive find / create | **0x1402e2c20**(mi, IDENT): direct child, else the config's child template (0x140c0f7c0). Only a hidden or dynamic template is instantiated (0x140c0e760 + 0x140c0ed30, otherwise it logs "Error creating dynamic child"). Then it recurses into visible children. |
| arms skin onto weapon | **0x1402e2910** (pointer at 0x141716880 in the base vtable 0x141715c58, static slot +0xc28; all 39 weapon vtables hold it; live caller unknown, 1.2): owner = `handle(weapon+0x98)`. If 0x1402acf00(owner) and owner `+0xff0` ≠ 0: `child = 0x140c10670(weapon+0x90, IDENT 'Arms' 0x142282698)`, then `0x140c112d0(child+0x38, owner+0xff0)`. |
| hide/show attachments | Weapon anim-event handler `vt+0x678` = 0x1402de1b0 (base; derived handlers jump to it). Two unnamed event types (0x142280c90 show, 0x142280c98 hide) call **0x1402de8c0** / **0x1402de720**(w, IDENT). Those use 0x1402e2c20 on the weapon mi, then the result of `w->vt[0x9c0]()` + 0x38. Script `CPlayerActorPuppetEntity::HideAttachmentOnWeapon` 0x140460790 calls 0x1402de720 on the right weapon. |
| new model instance | **0x140c0e9b0**() allocates 0x58 bytes and constructs (0x140c0a070). The VR controller code uses exactly this followed by `0x140c112d0(mi, cfg)` (0x140721b20). Stretch: **0x140c11370**(mi, &vec3). |

**Contradiction (unresolved): the static `CWeaponEntity` slot map disagrees with the weapon code's call
sites.** The base vtable is 0x141715c58; ctor 0x1402d1080 stores it at 0x1402d10a8. The 39 to 41 derived weapon
vtables were located through it, and every slot used here (+0x5f8, +0x678, +0x9c0, +0xaa0, +0xc28, +0xda8) gives the
same function in all of them. The call sites do not fit those functions:
* `vt+0x9c0` is called with only `this` by 0x14048ddbc (render view), 0x1402de763, 0x1402deb68 (inside the weapon
  step) and 16 other weapon sites. Each caller uses the result as a child object (`+0x38`). The static slot holds
  **0x14024c380**, a three-argument "serious time factor" setter that warns when its float is not 0.2.
* `vt+0xaa0` is called as `(w, bool inReach)` at 0x1402df213, but the static slot holds 0x1402d8aa0, which writes
  through its second argument (0x1402e13d0 stores an IDENT to `*param_2`).
* `vt+0xc28` is called as `(w, &out)` and its returned pointer is read as floats at 0x1402d6bd5, but the static slot
  holds 0x1402e2910 (`void(w)`).

So either the runtime vtables differ from the `.rdata` ones, or the 0x141715c58 layout is offset from what the code
dispatches. **Slot numbers for weapons below come from the static tables only.** The bodies and addresses of the
functions are solid (decompiled, field accesses match). Consequences:
* The mod hooks weapon functions **by address** (MinHook), never by vtable slot.
* It reaches the `Arms` child directly with `0x140c10670(weapon+0x90, 'Arms')`, as 0x1402e2910 does. The live caller
  of 0x1402e2910 is **unknown**.

Runtime check: log a live weapon's vptr and its entries at +0x9c0, +0xaa0, +0xc28, +0xda8 and +0x5f8, and compare
them with the static tables.

### 1.3 How the weapon and its arms are placed and drawn in VR
* **0x1402d8ad0** (R3 s2; static slot +0xda8) returns the weapon model's world matrix:
  * Desktop owner (`owner+0x25c8 == 0`): the view-model placement.
  * VR owner, one hand: controller `0x14043b920(vrd, out, en_bRightHandWeapon(+0x118) == 0)` (role 0 = right),
    composed with the weapon's offset from a virtual (`vt+0x768`), player `+0x2020`, and the world scale (`1/vr_fWorldScale`,
    0x14224cb98).
  * VR owner, two-handed grip: `0x1402e2d70(main, off, wp_eTwoHandedVROrentation)` (section 4).
  * In both VR cases it slerps from the last one-hand pose (`+0x26c..+0x284`, unreflected) over
    `pvp_fHandSnapLerpTime` since `+0x288` (unreflected).
* That matrix places the **root** of `X_Arms.mdl`. The gun follows `R_Weapon` and the arms follow their animation,
  both relative to that root. In VR the gun is therefore at the hand, and both arms keep the flat camera-relative
  pose around it.
* Drawing: render view 0x14048dad0 calls the right weapon's (`player+0xf90`) and left weapon's (`+0xf94`) `vt+0x628`
  before the view draws and `vt+0x620` after it. First-person weapons are drawn only in that player's own view
  (inferred from this bracketing).

### 1.4 Drawing an arms or hand model with no weapon
Requirements and the mechanism the engine already offers for each:

| need | mechanism |
|---|---|
| a model | The player's skin `player+0xff0` (`en_pmcFirstPersonArmsModel`, rec 0x1418623a8; SM +0xff8), or `Arms.mdl` (HandsWeapon). The default is `gpp_pmcDefaultFirstPersonPlayerArmsModel` (`CGlobalPlayerParams +0x28`, rec 0x141638c90), but how to reach `CGlobalPlayerParams` is **unknown**. |
| an instance | `mi = 0x140c0e9b0(); 0x140c112d0(mi, cfg); 0x140c11370(mi, {1/vr_fWorldScale ×3})`. This is what 0x140721b20 does for the hand-model slot. |
| a draw call | **0x140724620**(r, a, b, mi, const P7* placement, world). It is used by the controller draws 0x140722f30 and 0x140723160 with the renderer arguments of 0x140722e50(ctrl, r, a, b). So a hook on 0x140722e50 can draw any mod instance in the controllers' pass. |
| bone control | Register a mod `CMotionHandler` on the bare instance: `0x140bac1c0(0x140c10420(mi, 1), h)`. 0x140bf6b20 is only `renderable+0xd0 → this`, R9 dump. The apply ABI is R9 s3, as used by `src/se4/body.cpp`. |
| hide the other arm | No scale or surface option (section 0.2). Either drive both arms (one instance serves both hands), or park the unused arm **behind both eyes**: shoulder about 0.25 m behind the head centre, arm pointing backwards. All its vertices are then behind the near plane of each eye. Runtime check: also invisible in the PiP scope pass (R14) and in mirrors. |
| finger bones | Write the local rotations of `?_Thumb/Index/Middle/Ring/Pinky_01..03` in the handler (section 6.3). |

## 2. The native per-hand VR controller and hand model (`CVRController`)

Object: 0x108 bytes, two of them in vrd (`+0x3d8` right, `+0x4e0` left; ctor **0x140720210** called from the vrd
ctor 0x140439120, `+0x00` = vrd, `+0x10` = role).

| field | meaning (evidence) |
|---|---|
| +0x38 P7 (+0x54 in mixed reality) | Controller world pose, set by **0x1407234d0** from 0x14043b920 (tick 0x1407228c0 and render view 0x14048f4d9/0x14048f508). |
| `vrc_pmiController` +0x70, `vrc_pmiGhostController` +0x78 | Controller / ghost model instances (rec 0x141a59198..). The models come from `CGlobalVRParams` `gvrp_avrcmVRControllerModel` +0x60 (`CVRControllerModel`: `vrcm_iVRModelName` +0, `vrcm_pmcLeft/RightHandModel` +0x8/+0x10, ghosts +0x18/+0x20, **`vrcm_qvHandOffsetPlacement` +0x28**). |
| `vrc_preLaserPointerDot` +0x80, `vrc_pwldWorld` +0x88 | Laser dot and world. |
| **`vrc_bIsVisible` +0x94** | 1 from the ctor (0x140720290). 0 from **0x140721af0** (also hides the laser dot via `+0x80 vt+0x150`). Draw gate in 0x140722e50. |
| **`vrc_pmiHandModel` +0xe0** | Set in init **0x140721b20** only when `CGlobalVRParams+0xa8` (`gvrp_pmcHandModel`, rec 0x14164ab68) ≠ 0. NULL in SS4 and SM (R9 s1). Other `CGlobalVRParams` members: `gvrp_vHandOffset` +0xb0, `gvrp_qvHandOffsetPlacement` +0xbc, `gvrp_vHandsAnimFadeInTime` +0xd8. |
| +0xfc (unreflected) draw mode | Written each tick by 0x1407228c0. 2 = hand model (needs +0xe0 and VR viewer `+0x360 → +0xd0 == 0`). 1 = ghost (when player `+0x1f88` object `+0x1e0` ≠ 0; meaning unknown). 0 = controller (when controller-idle detection 0x140b6ad20 fires: `vr_bEnableControllerIdleDetection` 0x14224cb8c, `vr_fVRControllerIdleTime` 0x14224cb78; forced by `vr_bForceControllerVisible`). |
| +0x100 (unreflected) | 1 from the ctor. The tick keeps it only while that hand has no weapon and the other weapon fails 0x1402dd630 (meaning unknown), and clears it otherwise. It is also set by 0x140721c88. 0x1407219e0 uses it to pick the controller-type idle IDENT. |
| +0x104 (unreflected) | "finger animations started" latch for 0x140722380. |

Functions:
* **Tick 0x1407228c0** (per hand, from 0x1404399c0):
  1. If device slot 0x1d8 (**0x14043aac0**, unguarded R9 C1) ≠ 0, start the finger set once with **0x140722380**:
     `Thumb/Index/Middle/Pinky/Ring_Pose` (IDENTs 0x14228cda8/cd88/cd90/cd98/cda0) at strength 0, plus
     `Idle_Knuckles` (0x14228cd80) at 1, on the +0xe0 instance's anim queue.
  2. Pose the controller.
  3. Compute +0xfc and +0x100.
  4. If slot 0x1d8 ≠ 0, set each finger animation's strength to device **slot 0x1e0**(finger 0..4 =
     thumb, index, middle, ring, pinky; isLeft). Non-thumb values are multiplied by `vr_fSkeletonFingerPoseFactor`
     (0x1422493a0).
* **Draw 0x140722e50**(ctrl, r, a, b):
  1. Return if `+0x94 == 0 && vr_bForceControllerVisible == 0`.
  2. If `+0x100 == 0`: mode 2 → **0x140723160**, mode 1 → ghost. Otherwise → **0x140722f30** (controller model).
  3. Always flushes `r+0x1d78`.
* **Hand draw 0x140723160**: stretch `(±1, 1, 1)/vr_fWorldScale`, where **x = −1 for the left role** (a mirrored
  right-hand model). Placement **0x140720a30**(ctrl) = controller pose ∘ `vrcm_qvHandOffsetPlacement` of the model
  entry whose name is device slot 0x118 for that hand (lookup 0x14008c4b0 on `CGlobalVRParams`), with its position
  scaled by the instance stretch (0x1401c2da0). Then it calls 0x140724620.
* **Which controllers are drawn: 0x14048f8f0**(player, r, a, b), reached through a jmp at 0x140724b6f.
  * Skipped in cinema (VR viewer +0xd0) and while placing VBS pins (0x14043eba0 = `prj_bPlacingVBSPin`).
  * With no global hand model:
    * The right controller is drawn when vrd `+0x7c` (right-hand pointed entity) resolves.
    * Each **empty** hand is drawn unless the other hand's weapon has `WF_GRIPPED_WITH_TWO_HANDS`.
    * Three weapon classes count as an empty hand (type globals 0x14228d7e8, 0x14228e1a0 and 0x14228dbb0; by their
      neighbouring getters these are probably `CGadgetHolderWeaponEntity` and `CTypingWeaponEntity`, the third is
      unidentified).
    * Then it returns if 0x1402a5f30(player) > 0. Otherwise it falls through and draws both controllers.
* The Talos-only VR hand code (fingertip attachment `IndexFingerTip` 0x142286ab0 in 0x14047c4c0 and 0x14063e3e0;
  "Can't find index fingertip attachment for VR hand model"; hand-grab 0x14047c2b0) is gated by IsTalos 0x1400a8cc0,
  so it is dead in SS4/SM.
* Glove trackers (`CPlayerVRData::GetGloveTracker` 0x140438ea0 → `CHandsTracker`, `IsTracking` 0x140436900,
  `GetTrackingData` 0x140436850, enum `GloveHand` / `GloveFingers` `GF_ForeArm..GF_HandPinky3` at 0x141f4c0a0) are a
  calibrated-glove API. No SS4 content uses them (not scanned in depth) and they are not needed.

Verdict: the slot, placement and draw work as is. The finger driver needs a hand model with those five animations,
and none ships. A mod-owned `Arms.mdl` instance in `+0xe0` would be drawn, offset by the controller-type entry
(`Rift` for Index, R3 s10.1), and mirrored on the left. Mirroring a two-arm mesh with negative stretch is untested
(winding and lighting are **unknown**). The finger animations would not exist. So section 8 draws the instance
itself instead of using the slot.

## 3. The third-person body's hands in first person (option B) vs. the first-person arms (option A)

The body skeleton `SeriousSam.skl` has `Left/RightHand{Thumb,Index,Middle,Ring,Pinky}{1,2,3}`, `Left/RightHand`,
`Left/RightWeapon` and `ViewMover` (R9 s2). Showing it in first person needs:
* the view-owner hook on 0x140ca3280, call site 0x14048ddd4;
* the `FirstPersonAvatar` model tag (0x140c10e60 / 0x140c11260, IDENT 0x142286b08), which hides the head and shirt
  (R9 s4/s5B). What it hides at runtime is **unknown**.

| | A: first-person arms (`Arms.skl`) | B: third-person body hands |
|---|---|---|
| mesh made for close view | yes: a first-person mesh with per-character skins | no: third-person LOD, seen from about 20 cm |
| hand that holds the gun | native: the weapon's own animation already wraps the fingers on the grip, and fire/reload animate it | the body's third-person weapon animation; the first-person gun model's grip does not match, so the hand would need IK onto the FP gun and the FP `Arms` child would need hiding |
| free hand with fingers | mod handler on the weapon instance or the standalone instance (15 finger bones per side) | mod handler on the body (same bones, already registered by `body.cpp`) |
| arm/shoulder | IK from a virtual shoulder; parked arm behind the eyes | the real body shoulder, which is consistent and gives full-body presence |
| other players, mirrors | not visible (first-person only) | the same model others see |
| effort | moderate: one handler kind, instance ownership rules, controller-draw hook | higher: view-owner hook, tag check, body vs FP-gun alignment, head hiding for every pass |
| risk | weapon slot-map anomaly (avoided by hooking functions, 1.2); whether the `Arms` child evaluates through the parent's queue (s9) | which surfaces the tag hides; torso clipping into the camera |

**Choose A for the local first-person hands, and B (the body, via `body.cpp`) for what others and mirrors see.** B can
later become an optional "full body" first-person mode.

## 4. Per-hand weapons and the native two-hand grip

### 4.1 Ownership
* Player `en_henCurrentRightHandWeapon` +0xf90 and `en_henCurrentLeftHandWeapon` +0xf94 (rec 0x141861e68 /
  0x141861ec8; **SM +0xf98 / +0xf9c**). The desired slots are `en_iDesiredRight/LeftHandWeapon`. Scripts:
  `ClientSelectWeaponInRightHand` 0x14045c760, `…LeftHand` 0x14045c7b0, `ToggleDualWielding_*` 0x14045bcd0 /
  0x14045be00, `IsTwoHanded` 0x14045d260 (player) and 0x1402d1a60 (weapon; both read params +0x1b4).
* `en_bRightHandWeapon` weapon+0x118 (rec 0x141710d70) picks the controller role in 0x1402d8ad0 and in the step:
  role = `(+0x118 == 0)`, with 0 = right.
* Weapon params (rec 0x14170eca8..):
  * `wp_bTwoHanded` +0x1b4
  * `wp_dwrDualWieldRequirement` +0x1b8
  * **`wp_eTwoHandedVROrentation` +0x1bc** (enum 0x1417130c8: 0 `THVWO_VR_BANKING_UPRIGHT`, 1 `…_UPRIGHT_REVERSE`,
    2 `…_MAIN_HAND`, 3 `…_MAIN_HAND_REVERSE`, 4 `…_OTHER_HAND`, 5 `…_AVERAGE`)
  * `wp_bUsesSingleArmAnimations` +0x1c4
  * `wp_bVRRenderControllerWhenWeaponIdle` +0x210
  * **`wp_fHandSnapMinDistance` +0x214**
  * `wp_vVRHandsOffsetFromHead` +0x148
  * `wp_pwrWeaponRenderingParamsVR` +0x130
  * `wp_idPoseAdjusterPoseVR` +0x118
* Which SS4/SM weapons have `wp_bTwoHanded` set is **unknown** (the `.ep` structs were not decoded). Log it at
  runtime.

### 4.2 Grip decision: weapon step 0x1402de9a0
Static slot +0x5f8. Six derived steps call it directly and one jumps to it (0x14025ea64 … 0x1407cb7b3), so a hook
on the function address covers every weapon.
* **Bits in player `+0x2338`** (**SM +0x2340**; the same byte as `ClientAction.ubFireButtonStates`, R3 s4).
  * The one-off initial-fire check, guarded by `WF_CHECKED_INITIAL_FIRE` 0x800, tests `2 − (right != 0)`: bit 0 for
    the right weapon, bit 1 for the left weapon.
  * The grip test uses **`(right != 0) + 1`**: bit 1 (value 2) for a right weapon, bit 0 for a left weapon. So the
    grip button is the **other hand's fire bit**.
  * Note that for a single right weapon, bit 1 is also its alt-fire (vt+0x8b0 in the initial check). This is
    inferred.
* **Grab** (flag 0x20 clear; site 0x1402df219):
  1. `grip = 0x1402d60e0(w, *vt+0x6d8(w, &id))`: world placement of a weapon attachment whose IDENT a virtual returns.
     It is **inferred** to be `LeftHandGripEffector` (IDENT 0x142280af0, which the pistol model has); the slot target
     is unverified (1.2).
  2. `off = 0x14043b900(vrd, …, right != 0)`: the other hand's controller.
  3. `inReach = |off.pos − grip.pos| ≤ (params+0x214 ≥ 0 ? params+0x214 : CPlayerVRParams+0xcc)`.
  4. A virtual `vt+0xaa0(w, inReach)` is notified (site 0x1402df213; a highlight or "can grip" hook, target
     unverified, 1.2).
  5. If `(+0x2338 & mask) && inReach`, it stores the current one-hand pose at +0x26c..+0x284, sets **`+0x168 |= 0x20`**
     and `+0x288 = now`.
* **Release** (flag set; site 0x1402ded28): when the bit drops, it stores the current blended pose, clears 0x20 and
  sets `+0x288 = now`. The lerp back takes `CPlayerVRParams+0xc8`.
* **Blend 0x1402e2d70**(out, main, off, mode):
  * Position = main hand.
  * Forward = normalize(off − main). If it is shorter than about 0.03 it keeps the main hand.
  * Up by mode: 0/1 = no roll (Euler from the direction; 1 reverses it); 2/3 = the main hand's up; 4 = the off hand's
    up; 5 = the average of both.
* `WF_GRIPPED_WITH_TWO_HANDS` = 0x20 (WeaponFlags enum 0x141713650.., `meta_enum.py`). `en_ulWeaponFlags` (rec
  0x141711010) has reflection flags 0, so it is **not replicated**. The server computes its own grip from `+0x2338`
  (from ClientAction/ClientActionVR) and the replicated hands. Whether `+0x2338` reaches other clients is **unknown**.
* Natively the off-hand controller hides while gripping (0x14048f8f0, section 2).

### 4.3 Letting the off hand grab and release by squeezing
* Hook 0x1402de9a0 (SM 0x1402de4d0). For a two-handed weapon (`params+0x1b4`):
  1. Save `player+0x2338`.
  2. Replace only the grip-mask bit with the mod's **grip intent**.
  3. Call the original.
  4. Restore the byte.
* This touches nothing else, because the initial-fire block runs only once per weapon (0x800).
* Grip intent:
  * **Engage** on a squeeze press edge while the off hand is in reach. Reach is the same test as above; the mod can
    pre-check it with 0x1402d60e0 and the off controller, or just pass the held squeeze and let the native distance
    test decide.
  * **Hold** while the squeeze stays above about 0.35 (with hysteresis).
  * **Release** on squeeze release. Optionally also release when the hands are more than 1 m apart.
  * The edge rule follows eduke32 (section 7) and stops a held squeeze from grabbing when the hand passes the gun.
* Server and remote copies: use the same hook with the client's grip flags (`PoseFlags::LeftGrip/RightGrip` in
  `core/wire.hpp`, already defined). The server's weapon (and its barrel ray, R3 s2) then matches the client. Without
  the mod channel, fall back to the native bit.
* While gripped, do not map the off hand's squeeze or trigger to game actions, as eduke32 does. A "free hand" is
  simply "not gripped": the off arm returns to its controller (section 8).

## 5. Hiding the engine's controller models

* Per hand, set `vrc_bIsVisible` = 0 with `0x140721af0(vrd+0x3d8 / +0x4e0)` (SM 0x140721250). This is exactly
  `CPlayerVRData::HideRight/LeftVRController` (0x1404385d0 / 0x140438610; SM 0x140438750 / 0x140438790). It also
  hides the laser dot.
* Nothing else writes 1 except the ctor (field_refs over 0x140720000..0x140726000). It is still safest to re-assert it
  each tick while hands are on, and to restore 1 (the ctor value) when hands are turned off. There is no native
  "show" helper; 0x140720290 is the ctor store.
* Keep `vr_bForceControllerVisible` (0x1422aace4) at 0. It overrides `+0x94` in 0x140722e50 and forces mode 0 in
  0x140b6ad20.
* Alternative: hook 0x140722e50 (SM 0x1407225b0) and skip the native draw for the local player. The same hook is the
  draw point for the standalone hands (section 8.2).
* Keep device slot 0x1d8 returning 0. Otherwise 0x1407228c0 starts the knuckle finger animations on any +0xe0 instance
  (not ours: section 8.2 does not use the slot), and remote peers hit the R9 C1 crash path.

## 6. Finger input

### 6.1 OpenXR hand tracking
* `XR_EXT_hand_tracking` is already in the instance's wanted list (`src/xr/runtime.cpp`). Nothing uses it yet.
* `xrCreateHandTrackerEXT(session, {hand, XR_HAND_JOINT_SET_DEFAULT_EXT})`.
* `xrLocateHandJointsEXT(tracker, {baseSpace = that hand's grip action space, time = predicted display time})` gives
  26 `XrHandJointLocationEXT`:
  * 0 palm, 1 wrist
  * 2–5 thumb metacarpal, proximal, distal, tip
  * 6–10 index metacarpal, proximal, intermediate, distal, tip
  * 11–15 middle, 16–20 ring, 21–25 little, in the same order
* Per the spec, each joint's +Z points back along its bone (away from the tip) and +Y points out of the back of the
  finger. Use a joint only when its `ORIENTATION_VALID` and `POSITION_VALID` bits are set; otherwise hold the last
  value and fade to the fallback.
* `XR_EXT_hand_tracking_data_source`: chain `XrHandTrackingDataSourceInfoEXT{ {UNOBSTRUCTED, CONTROLLER} }` at create
  time and `XrHandTrackingDataSourceStateEXT` at locate time. That lets the mod accept controller-driven joints
  (Index under SteamVR or Monado) and know which source is active.
* `XR_EXT_hand_joints_motion_range` (optional) selects `UNOBSTRUCTED` versus `CONFORMING_TO_CONTROLLER`.
* Whether SteamVR and Monado (Envision build) expose these two extensions, and whether Monado's Index driver feeds
  `XR_EXT_hand_tracking` from the controller's capacitive sensors, is **unknown** here. Check at runtime with
  `xrEnumerateInstanceExtensionProperties` and `XrSystemHandTrackingPropertiesEXT`. Never require them.

### 6.2 Fallback from controller state (always available)
| finger | Index (`/interaction_profiles/valve/index_controller`) | Touch | Vive / WMR / simple |
|---|---|---|---|
| index | `trigger/value` (0→1 = 0.25→1 curl); `trigger/touch` without pull → 0.25, else 0 | same (`trigger/touch`, `trigger/value`) | trigger value or click |
| middle, ring, pinky | `squeeze/value` (capacitive grip curl); `squeeze/force` adds the last 10 % | `squeeze/value` | `squeeze/click` → 1 |
| thumb | `thumbstick/touch`, `trackpad/touch`, `a/touch`, `b/touch` → 0.7 (rest on top), else 0.1 | `thumbstick/touch`, `x/a/y/b touch` | trackpad touch |

The Index profile exposes no separate middle, ring or pinky curls in OpenXR core. Those need 6.1. Smooth each curl
(critically damped, about 60 ms). Weapon-holding hands ignore input curls (section 8). The current bindings
(`src/xr/input.cpp`) already have `trigger`, `trigger_touch`, `squeeze` (Index `squeeze/force`, which should change to
`squeeze/value` for curls, with force kept for the grab threshold) and `stick_touch`. They need `a/b/x/y touch` and
`trackpad/touch`.

### 6.3 Mapping onto bones
Reduce both sources to **flexion angles per joint** (and thumb abduction) before touching bones. That gives one code
path, a small network payload and no axis conventions to get wrong.
* From joints: θ = angle between consecutive bone directions (−Z of joint i and of joint i+1), signed by the finger's
  +X. Thumb: the metacarpal also gets abduction about the palm normal.
* From curls: θ_k = curl × maxAngle_k (for example 60°/90°/60° for _01/_02/_03; thumb 30°/40°/45°).

| OpenXR joints (θ between) | `Arms.skl` bone (FP) | `SeriousSam.skl` bone (body) |
|---|---|---|
| 2→3 (thumb metacarpal) | `?_Thumb_01` | `Left/RightHandThumb1` |
| 3→4 | `?_Thumb_02` | `…Thumb2` |
| 4→5 | `?_Thumb_03` | `…Thumb3` |
| 7→8 (proximal) | `?_Index_01` | `…Index1` |
| 8→9 (intermediate) | `?_Index_02` | `…Index2` |
| 9→10 (distal) | `?_Index_03` | `…Index3` |
| 12–15, 17–20, 22–25 | `?_Middle/Ring/Pinky_01..03` | `…Middle/Ring/Pinky1..3` |
| 1 (wrist) | `?_Hand`: keep the controller grip pose (stable), not the tracked wrist | `…Hand` |

Apply each angle as `local = rest_local · axisAngle(a_local, θ)`. The flexion axis is computed **once per skeleton at
runtime** from rest data (`SkeletonBone +0x48` quat / +0x58 local position, R9 s2):
* d = normalize(child local position) in the bone frame;
* palm normal n = cross(Index_01 − Hand, Pinky_01 − Hand) in the hand frame, with the sign flipped for the left side;
* a = normalize(cross(d, n)) carried into the bone frame.

Choose the sign so that positive θ moves the tip toward −n. Log the axes and the rest finger pose on first use. If the
rest hand is not flat, calibrate "open" as rest (runtime check). A relaxed native base pose is also possible: play
`Arms_Idle` on the standalone instance with the anim-queue calls used by 0x140722380 (0x140baf450 play,
0x140bafb10 strength).

## 7. eduke32_vr behaviour used as the design reference (GPL, design only)
* Hands are drawn separately from weapons. An empty hand gets a named pose chosen from buttons (grip+trigger fist,
  grip point, trigger, relaxed), or raw tracked joints. Priority is weapon hold pose, then tracked joints, then button
  pose. The holding hand uses the weapon's "hold" pose in the weapon frame, so the hand follows the gun.
* Two-hand: a weapon qualifies when its model has a support tag.
  * **Engage** only on the off hand's squeeze *press edge* within 15 cm of the support point.
  * **Release** on squeeze release, weapon change, invalid pose, or hands more than 1 m apart.
  * Aim = shortest-arc rotation from the palm→support direction to the main→off direction, applied to the main aim
    pose. Roll stays with the main hand.
  * The off hand is drawn at the weapon's support pose while engaged, and its grip input is blocked from the game.
* Trigger pull bends the index finger of the holding hand by a small amount, eased over about 80 ms.
* Multiplayer: head, both hands and the weapon frame travel in an unreliable pose message relayed by the server
  (≤ 90 Hz). Peers rebuild named or hold poses and do not receive raw tracked fingers. Poses older than 0.5 s are
  ignored.

The SS4/SM native grip (section 4) already matches the structure, with the fire bit instead of the squeeze. The
mod's `core/two_hand.hpp` (SS2 port) matches the aim math and is not needed for native two-handed weapons.

## 8. Recommended implementation

### 8.1 Arms ownership rules (local VR player, simulation thread, every tick)
* `R` = right weapon `handle(player+0xf90)`, `L` = left weapon `handle(player+0xf94)`. A weapon whose class is one of
  the three "empty-hand" types of 0x14048f8f0 counts as none.
* Right arm owner:
  * `R` if it exists, in its `X_Arms` instance;
  * otherwise the standalone instance.
* Left arm owner:
  * `L` if dual wielding, in its `_Arms_L` instance;
  * otherwise `R` if `R` is two-handed **and** `R+0x168 & 0x20`;
  * otherwise `R` with the left arm in **FREE** mode, when `R` exists;
  * otherwise the standalone instance.
* Every arm of every live instance that is not its owner's is **PARKED** (dual wield: the left arm of `R`'s instance
  and the right arm of `L`'s).
* The standalone instance is drawn only while it owns an arm.

Arm modes in the handler:

| mode | hand target | arm | fingers |
|---|---|---|---|
| WEAPON (holding hand) | native (animation keeps the hand on the gun) | 2-bone IK from the virtual shoulder to the animated `?_Hand`, then restore the hand's model-space rotation | native (fire/reload); optional trigger index curl |
| GRIP (off hand on a two-handed weapon) | native animated `?_Hand` on the gun (eduke32 "support pose"); blend in or out over `pvp_fHandSnapLerpTime`, timed from weapon `+0x288` | IK from the virtual shoulder | native |
| FREE | controller grip pose ∘ per-side hand offset (tuned once; the controller model's `vrcm_qvHandOffsetPlacement` for `Rift` is a starting value) | IK from the virtual shoulder | input curls (section 6) |
| PARKED | shoulder about 0.25 m behind the head centre, arm along +Z (backwards) | fixed | any |

* Virtual shoulders: the head pose (0x14043cb50, SM 0x14043cc90), offset down about 0.18 m and sideways ±0.18 m,
  yawed with the body (not with head roll).
* If the arm cannot reach, slide the shoulder toward the target. First-person views rarely show the shoulder.
* Reuse `core::retargetArm` from `body.cpp`. Write positions for `?_UpperArm` (the pose position result +0x1c is
  copied back by 0x140c11f90 for every bone not locked by +0x30).

### 8.2 Modules and hooks
* **`src/xr/hands.{hpp,cpp}`**:
  * Owns the hand trackers (6.1) and the controller-state fallback (6.2).
  * Produces per hand: `curls[5]` (θ per joint), thumb abduction, source (joints, buttons, none) and validity.
  * Runs on the XR frame and publishes a lock-free snapshot.
  * New bindings: Index `a/b touch`, `trackpad/touch`, `squeeze/value`.
* **`src/core/hand_pose.hpp`** (engine-agnostic, host-tested):
  * joints → angles;
  * curls → angles;
  * smoothing;
  * flexion-axis derivation from rest bones;
  * hold, grip and park targets;
  * shoulder model.
* **`src/se4/hands.cpp`** (option `hands`, default 0 until a headset test):
  * Sim-thread tick: chain onto the vrd tick, as `body.cpp` does.
    * Apply the ownership rules (8.1).
    * Track the weapon handles. For each live first-person weapon instance, register one mod `CMotionHandler` on
      `0x140c10420(weapon+0x90, 1)` with `0x140bac1c0`. Re-register when the handle changes or the handler's count
      drops to 0 (R9 s3 lifetime rules).
    * Create the standalone instance once: `0x140c0e9b0`, then `0x140c112d0(mi, player+0xff0)` (SM +0xff8), then
      `0x140c11370` with stretch `1/vr_fWorldScale`, then register its handler. Re-create it when `+0xff0` changes
      (skin change).
    * Hide the controllers (section 5).
    * Publish a pose snapshot: head and controller world poses, weapon matrices, ownership/modes, curls, grip state.
  * **Hook 0x1402d8ad0** (SM 0x1402d8720; one function shared by all weapon vtables). After the original returns,
    copy its output matrix per weapon. That is the root placement used to convert world targets into each weapon
    instance's model space (inverse of a uniformly scaled 3×4).
  * **Hook 0x1402de9a0** (SM 0x1402de4d0): grip-intent bit substitution (section 4.3). It runs for remote players too,
    with their replicated flags.
  * **Hook 0x140722e50** (SM 0x1407225b0):
    * For the local VR player's right controller, if the standalone instance is visible, call
      `0x140724620(r, a, b, mi, &rootPlacement, ctrl+0x88)`. Root = head pose, so `Root/ViewMover` sit at the eyes as
      in flat first person.
    * Skip the native controller draw while hands are enabled (this replaces or doubles the `+0x94` route).
    * Leave the camera-entity controllers (0x14062d770) alone.
  * Motion handler apply: read the newest snapshot only and never mutate game state (R9 5A.4).
    * Resolve the `Arms.skl` indices once per pose IDENT array.
    * Run FK on the incoming arrays.
    * Per arm, apply the mode (8.1). Then apply finger angles to the 15 finger bones of FREE arms.
    * Fade to native (return 0) if the snapshot is older than 0.25 s.
* **`src/se4/body.cpp`** (other players and mirrors):
  * Add finger angles from the replicated curls to `SeriousSam.skl`'s finger bones for **free** hands only.
  * When the replicated `LeftGrip`/`RightGrip` flag is set for a two-handed weapon, do not override that arm: the live
    `CHandsAdjuster` (player +0x26a8, R9) already holds the left hand on `LeftHandGripEffector`. Instead, aim the main
    hand with the native blend rule (`wp_eTwoHandedVROrentation`) so the third-person gun points along the two-hand
    line.
* **`src/core/wire.hpp` v2**: put `curl[2][5]` (uint8 θ fractions) and the thumb abduction in the 20 reserved bytes.
  `LeftGrip`/`RightGrip` already exist. Bump `kVersion` and keep v1 receivers (unknown version → native body). Other
  players then see both hands, the fingers and the two-hand grip through the body. First-person hands are
  local-only.
* **Device**:
  * Keep slots 0x1d8 and 0x1e0 at "no skeleton".
  * Keep reporting `Rift` per hand for Index (R3 s10.1; it is only the controller-model and offset key).

### 8.3 Data flow
```
XR frame:  joints / button states → xr::hands snapshot (curls, source)
sim tick:  vrd tick hook → ownership/modes, register handlers, grip intent, controller hide, publish
           weapon step hook → native grip flag 0x20 (both builds, local + server)
           0x1402d8ad0 hook → per-weapon root matrix
render:    0x140722e50 hook → draw the standalone arms
bone eval: mod CMotionHandler::apply (unknown thread) → arm IK + finger bones from the snapshot
network:   PosePacket v2 (head, hands, grip flags, curls) → body.cpp on every peer
```

### 8.4 Order of work
1. Controller hide plus logging. Log the `Arms.skl` parents, rest pose and lengths, and the live weapon slot map (1.2); on
   weapon change log `params+0x1b4/+0x1bc/+0x214` and the grip attachment's world pose.
2. A motion handler on the right weapon instance with the FREE left arm and fixed curls (no XR hands yet).
3. Standalone instance and draw hook.
4. Grip-intent hook.
5. `xr::hands` (buttons first, then `XR_EXT_hand_tracking`).
6. Wire v2 and body fingers.
7. SM pass.

## 9. Runtime checks (lab or headset, in order)
1. Does a handler registered on `weapon+0x90`'s queue move the **`Arms` child's** mesh? The child's evaluation may use
   the parent queue (0x140c11f90 takes the parent's queue when the parent entry's +8 bit 1 is set). If not, also
   register on `0x140c10420(child+0x38, 1)`.
2. Log `Arms.skl` parent links and rest pose (bone lengths, finger axes); check whether the rest hand is flat.
3. Is a parked arm invisible in both eyes, in the PiP scope (R14) and in mirrors?
4. The live weapon vptr and its entries at +0x5f8/+0x9c0/+0xaa0/+0xc28/+0xda8 (section 1.2 contradiction).
5. `wp_bTwoHanded` per weapon. The grip attachment name returned by `vt+0x6d8` / `vt+0x260` (expected
   `LeftHandGripEffector`).
6. When the player holds `CHandsWeaponEntity` (`Arms.mdl` as the weapon model) and whether it can be selected to free
   the right hand.
7. Which thread runs the apply for first-person instances (render vs. sim; R9 left this unknown).
8. OpenXR extension availability and data source per runtime (SteamVR, Monado/Envision), and the joint validity flags
   with Index.
9. The server with a VR client: does the weapon blend on the server match the client when grip flags travel?
10. Multiplayer: do remote VR bodies show the curls and the two-hand line?

## 10. SM port (SamSM.exe 628492)
Methods:
* **sig**: `tools/port_list.py` unique signature (length in bytes).
* **site**: `tools/port_sites.py` window.
* **vt**: same *static* slot in SM's base `CWeaponEntity` vtable **0x1416f8e18** (a static-to-static correspondence;
  the runtime slot map is unverified in both games, 1.2). Found as `p − 0x678`, where `p` is the only
  base vtable pointer to the exactly ported event handler 0x1402ddce0. Slots +0xc28 / +0xda8 / +0xaa0 agree with the
  ported functions.
* **chk**: `tools/sig_at.py` masked-byte match.

| Sam4 | SM | name | method |
|---|---|---|---|
| 0x140720210 | 0x14071f980 | CVRController ctor | sig 24 |
| 0x140721b20 | 0x140721280 | controller init (hand-model slot) | sig 24 |
| 0x140722380 | 0x140721ae0 | finger animations start | sig 24 |
| 0x1407228c0 | 0x140722020 | controller tick | call order (R9 s8) |
| 0x1407234d0 | 0x140722c30 | set controller pose | site 24 (back 16), chk 46/48 |
| 0x140722e50 | 0x1407225b0 | controller draw | sig 24, chk 56/56 |
| 0x140722f30 | 0x140722690 | controller model draw | sig 24 |
| 0x140723160 | 0x1407228c0 | hand model draw (mirrored) | sig 24, chk 56/56 |
| 0x140720a30 | 0x140720190 | hand placement (type offset) | sig 24, chk 60/60 |
| 0x140724620 | 0x140723db0 | draw model instance at P7 | site 48, chk 48/48 |
| 0x140721af0 | 0x140721250 | hide controller (+0x94 = 0) | sig 24 |
| 0x1404385d0 / 0x140438610 | 0x140438750 / 0x140438790 | script Hide Right/Left VR controller | sig 192 / 96 |
| 0x14048f8f0 | 0x14048fef0 | which controllers are drawn | site 24, chk 42/56 + the `CGlobalVRParams+0xa8` test at 0x14048ff63 |
| 0x140b6ad20 | 0x140b6b300 | controller idle test | sig 24 |
| 0x1402de9a0 | 0x1402de4d0 | weapon step (grip) | vt +0x5f8. Register allocation differs (chk 8/84), but the field pattern is identical: `+0x168 & 0x800`, `+0x2340`, `+0x1b4`, `+0x25d0`, `+0x1bc`, `+0x950`, `+0x214`, `+0xcc`, `& 0xffffffdf` |
| 0x1402df219 / 0x1402ded28 | 0x1402ded3d / 0x1402de84d | grab / release bit test | disassembly of the SM step |
| 0x1402df188 | 0x1402decac | read of `wp_fHandSnapMinDistance` | site 24 |
| 0x1402d8ad0 | 0x1402d8720 | weapon world placement | vt +0xda8. Same prologue shape with different registers |
| 0x1402de1b0 | 0x1402ddce0 | weapon anim-event handler | vt +0x678, chk 44/44 |
| 0x1402d8aa0 | 0x1402d86f0 | weapon function at static +0xaa0 | vt, chk 36/36 |
| 0x1402e2910 | 0x1402e26c0 | arms skin → `Arms` child | vt +0xc28, chk 39/40. It reads player +0xff8 and IDENT `Arms` **0x14225f7b0** (`cname_globals.py`) |
| 0x1402e2d70 | 0x1402e2b20 | two-hand blend | sig 24 |
| 0x1402e2c20 | 0x1402e29d0 | find/create child (recursive) | sig 24 |
| 0x1402e13d0 | 0x1402e1180 | `Walk` / `_SingleArm` choice | sig 24 |
| 0x140c10670 | 0x140c10df0 | find child | site, chk 28/28 |
| 0x140c108c0 / 0x140c11a00 | 0x140c11040 / 0x140c12180 | hide / unhide child | site, chk 28/28 |
| 0x140c10420 | 0x140c10ba0 | anim queue of instance | site, chk 40/40 |
| 0x140c0e9b0 | 0x140c0f140 | new model instance | sig 32 |
| 0x140c112d0 | 0x140c11a50 | set configuration | sig 32 |
| 0x140c11370 | 0x140c11af0 | set stretch | sig 192 |
| 0x140bac1c0 | 0x140bac8e0 | add motion handler to queue | sig 24 (R9) |
| 0x14043b900 | 0x14043ba00 | other-hand controller placement | sig 24, chk 39/40 |
| 0x14048ddbc | 0x14048e3cc | render-view `vt+0x9c0` call | site 24 |

SM members: player weapons **+0xf98 / +0xf9c**, arms skin **+0xff8**, fire bits **+0x2340**, vrd +0x25d0 (R3).
These are unchanged in SM:
* `CVRController` +0x94 / +0xe0;
* weapon +0x90 / +0x118 / +0x168;
* params +0x1b4 / +0x1bc / +0x214;
* `CPlayerVRParams` +0xc8 / +0xcc;
* `CGlobalVRParams` +0xa8;
* `WF_GRIPPED_WITH_TWO_HANDS` 0x20.

These were checked by meta records (`meta_members.py`, `meta_enum.py` on SamSM.exe). SM content:
* `Content/SeriousSamSM/00_All.gro` carries the same `Arms.mdl`, `Arms_Animations.ans`, `HandsWeapon.ep`,
  `GlobalVRParameters.rsc` and controller models.
* SM's `Arms.skl` has the identical bone IDENT list (diffed).

## 11. Unknowns (summary)
* The runtime `CWeaponEntity` slot map: call sites disagree with the static tables at +0x9c0, +0xaa0 and +0xc28
  (1.2). The live caller of the arms-skin setter 0x1402e2910 is unknown.
* Whether the `Arms` child evaluates with the parent queue's handlers.
* `Arms.skl` parents and rest pose.
* The thread of first-person bone evaluation.
* How the native single-arm animations hide the unused arm.
* Grip attachment name (`vt+0x260`).
* Which weapons are two-handed.
* `CHandsWeaponEntity` selection.
* Meaning of `CVRController +0x100`, player `+0x1f88 → +0x1e0`, 0x1402dd630 and 0x1402a5f30.
* The third empty-hand type 0x14228dbb0.
* Replication of player `+0x2338` to other clients.
* OpenXR hand-tracking extension support per runtime.
* Negative-stretch (mirrored) rendering of the native hand slot.
