# Gate R7: vehicles, rideables and the mech (Sam4.exe 616154, SamSM.exe 628492)

Static analysis only (private Ghidra copy `lab/ghidra_r7`, decompiles in `lab/r7/`, no game run). Addresses are
Sam4.exe PE VAs unless marked **SM** (SamSM.exe 628492). "vehicle" means a `CVehicleEntity`, "operator" means the
puppet that drives it (`CPuppetEntity::en_henOperator`). Offsets come from SE4 meta member records
(`tools/meta_members.py`, `tools/meta_class.py`) or from the decompiled code at the cited address. Unknown means
unknown. Conventions follow R3 (P7 = quat xyzw + pos; `vt+0xNN` = vtable slot).

New read-only tools: `tools/plcmd_handles.py` (player-command name → handle global → reader functions),
`tools/vtable_of.py` (function → vtable + slot, or read slots), `tools/port_list.py` (port a list of Sam4 VAs to
another build by the shortest unique wildcarded signature, re-checked with a longer one), `tools/meta_class.py`
(all members of a class; `--class` indexes every member run). Two cautions found while doing this:
* `vtable_of.py` walks back over text pointers, and SE4 vtables are often adjacent, so the reported start can be
  early. The `CVehicleEntity` vtable is **0x141a88380** (stored by the ctor 0x140757230); the walk from its step slot
  reports 0x141a88148. All vehicle slots below are relative to 0x141a88380.
* `field_refs.py` missed the stores in the one-instruction setters 0x14075e420/0x14075e430
  (`movss [rcx+0xcf4/0xcf0], xmm1`). They were found by disassembly. Treat "no writer found" from that tool as a
  lead, not as proof.

## 0. Summary

* **Native vehicle steering is analog and comes from the ordinary movement commands.** Gamepad LX/LY →
  `plcmdX±/plcmdZ±` → the operator's `pci_vOperatorRelMove` (player+0x22f0) → `SetDriveSteerRatioAndLookDir`
  (puppet vt+0xcb0, which is also a net RPC) → vehicle `en_vDesiredTempoAbs` (+0x108) → wheeled-vehicle behavior
  input step → desired steering ratio = −relMove.x, desired driving ratio = −relMove.z. All of these are floats in
  [−1, 1]. The virtual pad's left stick already drives every vehicle. It needs no new binding.
* The dedicated `plcmdVehicle*` commands (SteerLeft/Right, Accelerate*, Yaw*, Pitch*, Roll*, Lean*, Handbrake) are
  read only behind a global that nothing writes (Sam4 0x14228d3d0, SM 0x14226a590). They are **dead** in both
  builds, and the gamepad profile binds none of them.
* The steering chain is smoothed (exponential, with `vp_fSteering{In,Out}DampingFactor`) and rate-limited
  (`vsp_fWheelSteeringSpeed`). The visible steering wheel or handlebar bone is written every frame by the
  behavior's animate step into the vehicle's `CBonePathFollower` (vehicle+0xc68), from the *physical* steering
  angle. Injecting the hand angle as a stick value therefore turns the real wheel only with lag. For 1:1 grab mode,
  inject the steer (so physics and multiplayer stay native) **and** overwrite that bone's follower entry after the
  native animate step.
* Grab points are native data: most models carry `LeftHandSteeringHandle` / `RightHandSteeringHandle`
  attachments. The game's own rider IK (`CVehicleHandlingMotionHandler`, player+0x24b0) already pins the
  third-person hands to them. Bone and attachment world placements have callable native helpers (section 3).
* No VR-specific code was found in the vehicle, behavior, steering or rider-handling code (no `player+0x25c8`
  reads there). In VR the camera stays the head placement (R3). The riding view option must be kept first-person.

## 1. Vehicle input

### Commands (Lua `Databases/CommandTable.lua`, bindings)
`plcmdVehicleAccelerateForward/Backward`, `plcmdVehicleYawLeft/Right`, `plcmdVehiclePitchForward/Backward`,
`plcmdVehicleRollLeft/Right` (group "Vehicle"); `plcmdVehicleSteerLeft/Right`, `plcmdVehicleLeanForward/Backward`,
`plcmdVehicleHandbrake` (group "WheeledVehicle"). Only `DefaultKeyboardBindings.lua` binds them (W/S/A/D, Shift,
Ctrl, Space, Numpad). W/S/A/D are *also* bound to `plcmdZ∓`/`plcmdX∓`. `DefaultGamepadBindings.lua` and
`DefaultVirtualGamepadBindings.lua` bind only LX→`plcmdX±`, LY→`plcmdZ∓` (plus RX/RY look). No gamepad binding
exists for any vehicle command.

Handle globals (from the registration stubs, `plcmd_handles.py`): SteerLeft 0x14228d598, SteerRight 0x14228d5a0,
AccelerateForward 0x14228d588, AccelerateBackward 0x14228d590, YawL/R 0x14228d558/560, PitchF/B 0x14228d568/570,
RollL/R 0x14228d578/580, LeanF/B 0x142280b58/b60, Handbrake 0x1422932f8; `plcmdX+` 0x1422858b8, `plcmdX-`
0x1422858c0, `plcmdZ+` 0x1422858d8, `plcmdZ-` 0x1422858e0.

### The dead digital path
* Vehicle step **0x14075cb20** (vehicle vt+0x208; SM 0x14075c3b0): when no stop timer is set (vehicle+0xd78 is the
  sentinel at 0x1422566e0) **and** the 32-bit global **0x14228d3d0** ≠ 0, it reads the operator's command set
  (operator vt+0xea8) with the boolean test 0x1412e4560 and writes **±1.0** to desired steer +0xcf0, yaw +0xcf8,
  pitch +0xcfc, roll +0xd00 and drive +0xcf4 (0x14075ccd5..0x14075ce10).
* The same global gates `plcmdVehicleLeanForward/Backward` in the rider handler 0x14023d990 (0x14023e957), and
  `plcmdVehicleHandbrake` in 0x140a53510 (0x140a53550).
* 0x14228d3d0 (in .data, zero-initialised) has exactly these 3 RIP-relative reads, no RIP-relative write, no
  absolute pointer anywhere in the image, and no harvested registration record. SM: 0x14226a590, the same
  3 reads (0x14075c565, 0x14023e572, 0x140a539b0) and no writer or pointer. **Unless something writes it through
  a mechanism these scans miss, the dedicated vehicle commands are never read.**

### The live analog path (operator relMove → vehicle ratios)
1. **Alive input** `0x1404135c0` (controller vt+0x40; SM 0x1404134a0 from the SM controller vtable 0x141812308+0x40):
   relMove.x = clamp((`plcmdX+` − `plcmdX-`) + extra terms, −1, 1), the same for z with `plcmdZ`. The values come from the
   analog command reader 0x1412e3fb0, so stick deflection survives (whether the input layer applies a dead zone or
   curve first is not traced). Then `player vt+0x1290` → ClientAction → apply 0x140471680 → **player+0x22f0 =
   `pci_vOperatorRelMove`** (`CPlayerControlsInfo` at player+0x22c0, member +0x30; SM: pci +0x22c8 → relMove
   **+0x22f8**). The same function forwards the command set to the ride: `ride(player+0x75c)->vt[0xb90](cmds)`
   at 0x140414e78. For `CVehicleEntity` that slot is the empty stub 0x140044530.
2. **Operated-puppet step** `0x1402baa50` (vehicle vt+0xb88; SM 0x1402ba8a0) reads `operator = handle(+0x428)`,
   its relMove (+0x22f0/+0x22f8) and look (+0x22c0). It ends in `vt+0xba0(this, &look, &relMove, &tempo)` with the
   **raw** relMove (a unit-length-clamped copy feeds only the tempo output).
3. vehicle vt+0xba0 = `0x140742a30` (SM 0x140742190) → `vt+0xcb0(this, &relMove, &look)`.
4. vehicle vt+0xcb0 = **`0x1402c1490`** (SM 0x1402c1270; the native behind script
   `CPuppetEntity::SetDriveSteerRatioAndLookDir` 0x140288b70). When a network session exists it first sends an RPC
   (net call 0x140bb3ad0, descriptor 0x14231a660). Then, if an operator exists, it stores
   **`en_vDesiredTempoAbs` (+0x108) = drive/steer vector**, the look at +0x120, and sets puppet state +0x658 = 3.
5. **Wheeled-vehicle behavior input step** `0x140a4ff50` (`CWheeledVehicleBehavior` vt+0x30, vtable 0x141c68d98;
   SM 0x140a503b0, vtable 0x141c4d608) at 0x140a5042b..0x140a5047b: if the vehicle is not AI-driven
   (0x14075b5c0 → vehicle+0xcd4 == 0) and state +0x658 ∈ {2, 3}:
   `steer = −tempo.x (+0x108)`, `drive = −tempo.z (+0x110)`. Otherwise the AI path runs (0x140c37ca0 on
   vehicle+0x1c8). Then **0x14075e420(vehicle, drive) → +0xcf4** and **0x14075e430(vehicle, steer) → +0xcf0**.
   SM: 0x14075dcc0 → **+0xcfc**, 0x14075dcd0 → **+0xcf8** (all vehicle ratio fields are +8 in SM).
6. **Smoothing** in the vehicle step (0x14075cffb..0x14075d0a3): actual = desired − k·(desired − actual), with k from
   0x141404c30 (called with a negative argument; taken to be exp(−damping·dt)). The damping is `vp_fSteeringOutDampingFactor`
   (`CVehicleParams` +0x67c) or `vp_fSteeringInDampingFactor` (+0x680), chosen by direction. Yaw, pitch and roll
   use +0x684..+0x698, driving uses +0x69c. Actual steering ratio is **+0xcdc**, driving ratio is +0xce0 (SM +0xce4/+0xce8). The script
   getters agree: `GetSteeringRatio` 0x140756220 reads +0xcdc (SM 0x140755ad0 reads +0xce4), `SetDesiredSteeringRatio`
   0x140755990 writes +0xcf0.
7. **Steering parts** (vehicle `en_pvsSteering`, +0xc50 / SM +0xc58; created by the steering params' factory
   vt+0x80 in vehicle init 0x14075b780):
   * `CGroundSteering` (vptr 0x141c6dd30, size 0x30) step **0x140a569b0** (vt+0x30; SM 0x140a56de0):
     target = vt+0x50 (= `vsp_fMaxSteeringAngle` +0x70 in degrees→radians × `vsp_pgrMaxSteeringAngleRatio` +0x78 graph by
     speed) × ratio(0x14075b0e0 → +0xcdc). Steering angle +0x28 moves toward it at ≤ `vsp_fWheelSteeringSpeed`
     (+0x84, deg/s) × dt and is pushed to each steered wheel through 0x140a989b0 (wheel+0x110).
     vt+0x40 = 0x14051a280 returns +0x28 (current angle), vt+0x48 = 0x140a55d50 returns `vsp_fMaxSteeringAngle` in radians.
   * `CTwoWheelVehicleSteering` (vptr 0x141c6fb08, size 0x38) step **0x140a58720** (vt+0x38; SM 0x140a58b60). It uses
     the same vt+0x40/+0x48 and also lean (`vsp_fMaxLeaningAngle` +0x98 …). Params: `vsp_idFrontWheel` +0x60,
     `vsp_fMaxSteeringAngle` +0x70, `vsp_fSteeringIn/OutAcceleration` +0x84/+0x80, `vsp_fMaxSteeringSpeed` +0x88.
   * `CDualDrivetrainSteering` (vptr 0x141c6d4c0, factory 0x140a55d00), plus helicopter and airplane variants
     (0x141c6ee50 is one of them, size 0x50, not attributed). Content uses only the ground and two-wheel types
     (section 5).
8. Throttle/brake: `drive` from relMove.z (LY). The handbrake flag (+0x6c) of the part stepped by 0x140a53510 (a
   propulsion/engine part with throttle press/release durations at params +0xa0/+0xa4; class not pinned), when the dead
   flag is 0, is `operator+0x10c > 0`, i.e. the operator's own `en_vDesiredTempoAbs.y`. That is probably the jump/up
   command (`plcmdY+`, pad A), but this is **not verified**.

**Answer to Gate R7 Q1:** steering is analog end to end (stick → relMove → tempo → desired ratio float →
smoothed ratio → rate-limited angle). The PWM fallback in PLAN is not needed.

### Vehicle entity and part classes
`CVehicleEntity` (base `CPuppetEntity`, size 0xd80, new 0x140755510, ctor 0x140757230, vtable 0x141a88380; SM
vtable 0x141a6c9f8 with the same slot layout): step vt+0x208 = 0x14075cb20, post-step vt+0x210 = 0x14075d470
(SM 0x14075cd00), init 0x14075b780. Members (`meta_class.py`): `en_apvpParts` +0xc38, `en_pvpPropulsion` +0xc48,
`en_pvsSteering` +0xc50, `en_avbBehaviors` +0xc58, `en_mhSkeletonAdjuster` +0xc68 (actually a `CBonePathFollower`,
see 2), `en_ulVehicleFlags` +0xc80. SM: +0xc58 steering, +0xc70 adjuster.
Behaviors: `CWheeledVehicleBehavior` (input vt+0x30 0x140a4ff50, init vt+0x18 0x140a4f180, animate vt+0xa8
0x140a509d0), `CTwoWheeledVehicleBehavior` (balance/lean: `vbp_fMaxLeaningAngle` +0x60 …),
`CWeaponizedVehicleBehavior`, `CVehicleScriptBehavior` (Lua). Parts: `CVehicleWheel` (vptr 0x141ca42e8, pose
vt+0x70 = 0x140a96a80), suspension, engine, transmission. Propulsion `en_pvpPropulsion` +0xc48 (post-step calls its
vt+0x38). 0x140a53510 sits in the vtable run at 0x141c6b0a0 (pointer at 0x141c6b150); its class and slot are not pinned.
Other driven puppets: `CMechPuppetEntity` (Mech.ep, MechMilitary.ep, SM Babymech.ep). Its walking reads operator
relMove at 0x14074e550 (operator+0x22f0/+0x22f8 → +0xf74 "moving") and reaches vt+0xcb0 through 0x140742a30 /
0x140745410. Ridden creatures use `CPSRiding` (state, `es_henRide` +0x28, `es_idSeat` +0x38) and
`PlayerRideSelection.lua` (rear mount, "NoRiding", stun for Khnum/Reptiloid). They are driven by the same operator
relMove/look through the operated-puppet step. `CHovercraftPuppetEntity` and `CCombatDronePuppetEntity` exist in
code. No content was found for them except SM `Hoverboard_DecoyBase.ep`, whose class is not resolved.

## 2. Steering bone data and who animates it

| member | class | offset | owner / use |
|---|---|---|---|
| `wvsas_idSteeringWheelBone` | `CWheeledVehicleSkeletonAdjusterScheme` (CResource) | +0x60 IDENT | rec 0x141c66cb8 (SM 0x141c4b528) |
| `wvsas_fMaxSteeringWheelAngle` | same | +0x68 FLOAT (deg) | rec 0x141c66d18 |
| `wvsas_idSpeedometerNeedleBone` / `wvsas_fMaxSpeedometerNeedleAngle` | same | +0x70 / +0x78 | |
| `wvsas_bUseHandlebar` | same | +0x7c BOOL | rec 0x141c66e58. **No code reader found** in the vehicle/behavior ranges. Content data only. |
| scheme pointer `vbp_pwvsasSkeletonAdjusterScheme` | `CWheeledVehicleBehaviorParams` | +0x68 | |
| `vb_qInitialSteeringWheelBoneRotation` | `CWheeledVehicleBehavior` | +0x94 Quaternion4f | speedometer +0xa4 |
| `vp_idSteeringBone` | `CVehicleWheel` | +0x58 IDENT | the *wheel's* steering-knuckle bone (on the chopper this is the fork and bars) |
| `vp_qvInitialSteeringBonePlacement` | `CVehicleWheel` | +0x7c QuatVect | |
| `sosd_idSteeringBone` / `sosd_qvInitialSteeringBonePlacement` | `CVehicleWheel_SaveData` | +0x40 / +0x74 | the savegame copy of the two above |
| `vpp_idSteeringBone` | `CVehicleWheelProperties` | +0x28 | the content-side value |
| `mh_idSteeringWheelBone` / `mh_fSteeringWheelAngle` | `CWheeledVehicleSkeletonAdjuster` (CMotionHandler, record 0x141a89718, size 0x38, vtable 0x141a896a8, pose fn 0x14075ec20) | +0x18 / +0x20 | not the path used by current vehicles (see below). Where it is instantiated was not traced. |

SM offsets are identical for all of the above (checked with `meta_members.py` on SamSM.exe).

**Cockpit wheel or handlebar bone** (`CWheeledVehicleBehavior`):
* Init `0x140a4f180` (SM 0x140a4f5e0): if the scheme exists it registers the steering-wheel and speedometer bones
  with the vehicle's bone follower (`0x140076ca0(vehicle+0xc68, scheme+0x60)`, SM 0x140076fa0). It reads the bone's
  default model-space matrix with `0x140bf9250(model = vehicle vt+0xc8, out, ?, boneIdent)` (SM 0x140bf9b70) and
  stores the rotation in +0x94 (and the speedometer's in +0xa4).
* Animate `0x140a509d0` (vt+0xa8, called from the vehicle post-step 0x14075d470; SM 0x140a50e30), when model
  renderable (vehicle+0x1d0) ≠ 0 and scheme+0x60 is not the empty IDENT:
  `angle = steering.vt[0x40]() / steering.vt[0x48]() × wvsas_fMaxSteeringWheelAngle × π/180`
  (current physical steering angle ÷ max steering angle, scaled to the wheel's max). It calls
  `0x14007a960(bpf, bone, 1)` and `0x14007a810(bpf, bone, 2)` (mode and flags), builds `q = q_init(+0x94) ⊗
  Euler(H = angle, 0, 0)` (Euler→quat 0x1411458e0, SM 0x1411460f0) and writes it with **`0x14007a930(bpf, bone, &q)`**
  (SM 0x14007ac10). So the wheel turns about the bone's heading axis in its initial frame. The speedometer needle uses
  |forward speed 0x14075aa60| ÷ 0x140a53cf0(propulsion +0xc48), taken to be the max speed, × `wvsas_fMaxSpeedometerNeedleAngle`.
* **Wheel steering bone** (`CVehicleWheel` pose `0x140a96a80`, wheel vt+0x70; SM 0x140a971c0): if
  `vp_idSteeringBone` (+0x58) is set, `q = q_init(+0x7c) ⊗ Euler(H = wheel+0x110)` → `0x14007a930`. Then the wheel
  bone itself (+0x48) gets a full placement through `0x14007a890` (SM 0x14007ab70).
* The follower at vehicle+0xc68 is a **`CBonePathFollower`** (class record before 0x1415942c0, size 0x40, vtable
  0x141596858, ctor 0x1400768b0, created in vehicle init 0x14075b780). Its entries are an array at +0x18 (count
  +0x20), 0x40 bytes each: +0x00 bone IDENT, +0x10 quat, +0x20 pos (QuatVect up to +0x2c), +0x2c flags (OR-ed by
  0x14007a810), +0x30 mode (0x14007a960). The script wrapper `CVehicleEntity::GetScriptedBonePathFollower` 0x140756970
  (SM 0x140756230) wraps exactly vehicle+0xc68. `CScriptedBonePathFollower::SetBonePlacement` 0x1400749b0 (SM 0x140074cf0) writes
  entry +0x10..+0x2c. SM's own Lua uses that API on vehicles (`VehicleSM_Tank.lua` turret/barrel,
  `VehicleSM_MLUV.lua` machine gun), so writing follower entries is a supported way to pose vehicle bones.

**Answer to Gate R7 Q2:** the bone angle is derived from the physical steering angle, which lags the input
(exponential ratio smoothing plus rate limit). Driving the input from the hands does **not** give 1:1 bone motion.
For 1:1, write the follower entry for the steering bone **after** the behavior animate step (vt+0xa8) each frame.
Physics still takes its steer from the injected stick value.

## 3. Bone and attachment world placement at runtime

* Bone world placement **`0x1400785d0(out P7, entity, boneIdent)`** (native of script `GetBonePlacementAbs`
  0x140075270; SM 0x1400788e0 called by SM 0x1400755b0): model instance = `entity->vt[0xc8]()`, skeleton/pose =
  `model+0xd0`, bone matrix = **`0x140c2bb90(time 0x1411223e0(), model+0xd0, boneIdent, &mat3x4, 0, 0)`** (SM
  0x140c2c180) in model space, composed with the model placement at model+0x3c (quat) / +0x4c (pos). It returns
  identity when the bone or model is missing. Variants: object space 0x140078b10, relative 0x140078e30. Default/bind
  matrix: 0x140bf9250 (SM 0x140bf9b70). `IfEntityHasBone` 0x1400750a0 shows the bone lookup (0x140c288d0 /
  0x140c282a0). **Unknown:** whether 0x140c2bb90 already includes this frame's bone-follower override, i.e. whether it
  returns the post-animation pose.
* Attachment world placement: `0x14024cb30(entity, out, attachIdent)` (native of `CBaseEntity::GetAttachmentAbsolutePlacement`
  0x140247c50; SM 0x14024c750 via SM 0x1402477f0) = `entity->vt[0x388](entity, ctx, ident, &mat)` → QuatVect
  (0x141146cd0, SM 0x1411474e0). `ctx` = global 0x14239c4e0 when 0x1410ca020() is true, else 0 after an error print.
  Model-level form used by the rider IK: `0x140bf7ec0(model, out, time, attachIdent, 1)` (SM 0x140bf87c0).
* IDENT from a C string: `0x1410c65f0(&out, "name")` (SM 0x1410c6da0). An IDENT is an interned string pointer:
  follower entries and these helpers compare pointers. Handle → entity: 0x141112c50 (SM 0x141113500).
* Per-frame recipe: vehicle = `handle(player+0x75c)` (`en_henRide`; check it is a `CVehicleEntity`, class global
  0x14228d3c0, with `0x1410df8c0(class, 0x14228d3c0)` as done at 0x14023d9d7). Bone = scheme
  (`behavior+0x10 → params+0x68 → +0x60`) or wheel +0x58. Grips = attachments `LeftHandSteeringHandle` /
  `RightHandSteeringHandle`.

## 4. Driver seat, hand IK, camera and VR

* **Seat:** `pp_csoSeats` (`CPuppetParams` +0x2d8) of `CPuppetSeatData` (`psd_idSeatName` +0x8, `psd_idSeatAttachment`
  +0x10, `psd_sfSeatFlags` +0x20, `psd_psvPreferredSeatView` +0x24, `psd_idRiderAnimRideNotMoving/Moving` +0x30/+0x38,
  …). Every wheeled vehicle .ep names seat `DriverSeat` with rider anims `Sitting` / `Driving` (`Driving_Motorcycle`
  on the chopper). Rider links: `en_henRide` +0x75c, `en_idSeat` +0x770, `en_henOperator` +0x428, `en_eaRiders` +0x2a8.
* **Native hand IK onto the real wheel:** the player owns `en_pmhVehicleHandling` +0x24b0
  (`CVehicleHandlingMotionHandler`, size 0x198; SM +0x24b8) and `en_pmhPuppetRidingMotionHandler` +0x24b8 (SM
  +0x24c0). Handler members: `mh_wvdhLeftHands` +0x28 / `mh_wvdhRightHands` +0x60 (`CVehicleDriverHand`:
  `vdh_acbdArmChain` +0, `vdh_pikhIKSolver` +0x10), pose groups `mh_wvrpg{No,Single,Dual}WieldingPoseGroupTP`
  +0xa8/+0xd8/+0x108. Scheme `ppp_pwvhsWheeledVehicleHandlingScheme` (`CPlayerPuppetParams` +0x958 →
  `CWheeledVehicleHandlingScheme`: `wvhs_idLeftHandInfo` +0x60, `wvhs_idRightHandInfo` +0x88 (`CHandInfo`:
  `whi_idShoulderBone` +0, `whi_idWristBone` +8, `whi_idPalmAttachment` +0x10, `whi_aidRotationAdjustBones` +0x18),
  `wvhs_ctMaxIKIterations` +0xb0).
  Step **0x14023d990** (SM 0x14023d5b0), run only when the ride is a `CVehicleEntity`:
  * Body steer and lean blend: steer pose = clamp(−3 × ratio(+0xcdc), ±1), smoothed into +0x160. Lean = speed ÷ max
    (or the dead digital Lean*), smoothed into +0x164. Both feed the pose groups (0x14023f8b0 / 0x140941830).
  * Per hand (loop at the end): target = **vehicle model attachment `LeftHandSteeringHandle` / `RightHandSteeringHandle`**
    via 0x140bf7ec0(vehicle+0x1e0, …, 1), blended with the animated hand pose by a weight, written into the IK
    target (+0x18 quat, +0x28 pos) and solved by 0x14023c010 (SM port not found by signature). The attachments sit on
    the steering assembly, so the native third-person hands already follow the real wheel or bars.
* **Camera:** the first-person view is `player vt+0x340` mode 0 (R3). With vrd it is the head placement (anchor ∘
  device slot 0x80), so it is unaffected by riding. The view mode comes from `0x1404789f0` (player+0x24e0): while
  riding (0x1402ad380) it is `prj_iGameOptionPreferredRidingView` (0x1422739cc), otherwise 0x1422739c8, or a per-profile
  value at +0x40/+0x44. Only the local copy is chosen this way. The enum value for first person is **unknown**. Riding
  FOV: `plr_fFOVRideOverride` (0x142248854), `pp_fVehicle1stPersonCameraFOVOverride` (`CPlayerStateCamera` +0x1c),
  `pp_pscVehicleOverTheShoulderCamera`. Operated puppets build their own view from the operator's look
  (0x1402a9440 / 0x1402a95f0 read operator pci +0x22c0).
* **CPlayerVRData while driving:** the anchor update 0x14043f180 follows the player entity placement (vt+0xa8). The
  seated player moves with the vehicle, so the anchor rides along. No ride check was found in the vrd code (no +0x75c
  reads in 0x14043xxxx). `player+0x25c8` is read nowhere in the vehicle (0x14074..0x14075), behavior/steering
  (0x140a4..0x140aa) or rider-handler (0x14023) code. The +0x25c8 reads that do exist near ride code (0x1405d5520, a
  scripted move-to-position state; 0x140a42a80, input-hint UI) are not vehicle logic. **No VR-specific vehicle code
  exists.** Whether the turn handler 0x14044e880 should run while seated is a mod decision (the anchor yaw is
  independent of the vehicle).

## 5. Vehicle catalogue (content scan of `00_All.gro` + `50_<build>_00_All.gro`; files extracted only into `lab/r7/content`)

Steering-wheel data is read from each .ep's `CWheeledVehicleSkeletonAdjusterScheme` record, matched as the field
sequence IDENT/FLOAT/IDENT/FLOAT/BOOL resolved through the file's IDNT pool (a heuristic; the binary meta format was
not fully parsed). Handle attachments come from a byte scan of the .mdl.

| game | entity (.ep) | steering / behaviors | steering-wheel bone (`wvsas_`) | max deg | bUseHandlebar | hand attachments in model |
|---|---|---|---|---|---|---|
| SS4 | `ATV_01.ep` (Atv_02.mdl) | Ground, Wheeled | `Handlebar` | 25 | 1 | yes |
| SS4 | `Bike_Chopper.ep` | TwoWheel, Wheeled + TwoWheeled | none (empty IDENT; speedometer only, 200°). Bars turn with the front wheel's `vpp_idSteeringBone` = `Steering` | (wheel `vsp_fMaxSteeringAngle`) | 0 | yes |
| SS4 | `CombineHarvester.ep` | Ground, Wheeled | `Steering` | 180 | 0 | yes |
| SS4 | `Tractor.ep` | Ground, Wheeled | `Steering` (the 616154 patch .ep; the base .ep has no `Steering` IDENT) | 180 | 0 | not found in the scanned models (SM Tractor*.mdl); SS4 model not scanned |
| SS4 | `Mech.ep`, `MechMilitary.ep` | `CMechPuppetParams` (walker) | none; IDENTs only `Seat`, `Head`, `ViewMover`. No lever or joystick bone found. | – | – | – |
| SM | `SpecialATV{,_Blue,_Red,_White}.ep` | Ground, Wheeled | `Handlebar` | 25 | 1 | yes |
| SM | `Snowmobile_v2.ep` | Ground, Wheeled, Script (`VehicleSM_Snowmobile`) | `Handlebar` | 20 | 1 | yes |
| SM | `Crane.ep`, `Crane_LowCamera.ep` | Ground, Wheeled | `Handlebar` | 25 | 1 | yes |
| SM | `MLUV.ep` (SUV model) | Ground, Wheeled, Script (`VehicleSM_MLUV`) | `Steering` | 180 | 0 | not found |
| SM | `Tractor.ep`, `TransportCart.ep` | Ground, Wheeled | `Steering` | 180 | 0 | not found |
| SM | `Tank.ep`, `Tank_SV.ep` | Ground, Wheeled, Script (`VehicleSM_Tank`) | none | – | – | no |
| SM | `Babymech.ep` | `CMechPuppetParams` | none | – | – | – |
| SM | `Hoverboard_DecoyBase.ep` | not a vehicle-params file (class not resolved) | – | – | – | – |

Rideable creatures (SS4/SM) follow `PlayerRideSelection.lua`. No rein or saddle bone was identified (not
searched per creature model).

## 6. Recommended implementation

### (a) Thumbstick driving (always available)
* Do nothing vehicle-specific: the virtual pad's left stick already produces analog steer (LX) and throttle/brake
  (LY) through `plcmdX±/plcmdZ±`. This works in single player, on hosts and for clients, because the vehicle input
  travels as the operator's ClientAction relMove and the native SetDriveSteer RPC (0x1402c1490).
* Do not add bindings for `plcmdVehicle*`: they are not read (dead flag 0x14228d3d0 / SM 0x14226a590).
* Detect "driving": `handle(player+0x75c)` is a `CVehicleEntity` and its operator `handle(vehicle+0x428)` is the
  local player. Use this to swap pad semantics (for example trigger → LY throttle when the vehicle has no weapon), to
  suppress native VR stick turning (0x14044e880) while seated, and to force the riding view to first person.

### (b) Grab mode (real wheel or handlebars)
1. **Per frame (simulation thread):** vehicle as above. Steering bone = scheme `wvsas_idSteeringWheelBone`
   (behavior +0x10 → params +0x68 → +0x60). If it is empty, fall back to the front wheel's `vp_idSteeringBone`
   (chopper). Max angle = `wvsas_fMaxSteeringWheelAngle` (deg); for the chopper fallback use the steering
   `vt+0x48()` (radians). Bone world P7 = 0x1400785d0. Grips = `LeftHandSteeringHandle` / `RightHandSteeringHandle`
   (0x14024cb30). Rim radius = in-plane distance from the bone origin to a grip. `bUseHandlebar` selects the
   bar model (grab the two grip points) or the wheel model (grab anywhere on the rim).
2. **Axis:** the native rotation is `q_init ⊗ H(angle)`, so the steering axis is the bone's local heading axis.
   In world space that is the bone world rotation applied to the engine's heading axis (taken as +Y by the SE4
   Euler H/P/B convention; check that sign once in the lab). `SteerAxis.zero` can be any in-plane vector, because
   `core::SteeringGrab` integrates deltas.
3. **Physics and multiplayer:** steer = `SteeringGrab::steer(max)` (+1 = left) → virtual pad **LX = −steer**
   (native ratio = −relMove.x, and a positive ratio turns the wheel bone by +H). If the stick is outside its dead zone
   it wins. Releasing the grip hands control straight back to the stick. Expect the vehicle to respond with the
   native lag (damping and rate limit). That is native behaviour and is left as is.
4. **Visual 1:1 lock:** post-hook `CWheeledVehicleBehavior` animate **0x140a509d0** (SM 0x140a50e30; wheel case:
   wheel pose 0x140a96a80 / SM 0x140a971c0). While a hand holds the wheel, overwrite the follower entry with
   `0x14007a930(vehicle+0xc68, boneIdent, q_init(+0x94) ⊗ H(handAngle))` (SM: follower +0xc70, setter 0x14007ac10;
   wheel case: q_init = wheel +0x7c). This uses the same API the game's own Lua uses (SetBonePlacement). It is visual
   only and needs no replication; remote peers see the native (lagging) angle.
5. **Hands:** while grabbing, pin the VR body IK hands to the grip attachments (or to the hand position projected onto
   the rim). The native rider IK (0x14023d990) already targets the same attachments for third-person bodies.
6. When no hand is held, call `SteeringGrab::follow(nativeAngle)`, with nativeAngle = steering `vt+0x40() / vt+0x48()`
   × max (this is the animate step's own formula), so a re-grab starts from the visible wheel position.
7. Lean, handbrake, yaw, pitch and roll: there is no live native command. Body lean is animation-only (rider handler
   +0x164) and two-wheeler lean is physics in `CTwoWheeledVehicleBehavior`. PLAN's "body lean → Lean*" has **no native
   effect** in these builds. Handbrake probably follows the operator's up-tempo (jump); verify in the lab before binding a squeeze to it.

### SM 628492 port table (signature-unique, checked by .pdata start/size and call order where noted)
| function | Sam4 | SamSM |
|---|---|---|
| vehicle step / post-step | 0x14075cb20 / 0x14075d470 | 0x14075c3b0 / 0x14075cd00 |
| CVehicleEntity vtable | 0x141a88380 | 0x141a6c9f8 |
| wheeled behavior input / animate / init (vtable) | 0x140a4ff50 / 0x140a509d0 / 0x140a4f180 (0x141c68d98) | 0x140a503b0 / 0x140a50e30 / 0x140a4f5e0 (0x141c4d608) |
| desired drive / steer setters | 0x14075e420 (+0xcf4) / 0x14075e430 (+0xcf0) | 0x14075dcc0 (+0xcfc) / 0x14075dcd0 (+0xcf8) |
| SetDriveSteer (vt+0xcb0) / vt+0xba0 / operated step (vt+0xb88) | 0x1402c1490 / 0x140742a30 / 0x1402baa50 | 0x1402c1270 / 0x140742190 / 0x1402ba8a0 |
| ground / two-wheel steering step | 0x140a569b0 / 0x140a58720 | 0x140a56de0 / 0x140a58b60 |
| wheel pose / wheel set steer | 0x140a96a80 / 0x140a989b0 | 0x140a971c0 / 0x140a990f0 |
| bone follower add / set rot / mode / flags / set placement | 0x140076ca0 / 0x14007a930 / 0x14007a960 / 0x14007a810 / 0x14007a890 | 0x140076fa0 / 0x14007ac10 / 0x14007ac40 / 0x14007aaf0 / 0x14007ab70 |
| bone abs placement / skeleton bone matrix / bind matrix | 0x1400785d0 / 0x140c2bb90 / 0x140bf9250 | 0x1400788e0 / 0x140c2c180 / 0x140bf9b70 |
| entity attachment abs / model attachment | 0x14024cb30 / 0x140bf7ec0 | 0x14024c750 / 0x140bf87c0 |
| IDENT from string / handle→entity / Euler→quat / mat→QuatVect | 0x1410c65f0 / 0x141112c50 / 0x1411458e0 / 0x141146cd0 | 0x1410c6da0 / 0x141113500 / 0x1411460f0 / 0x1411474e0 |
| rider handling step / handbrake part step | 0x14023d990 / 0x140a53510 | 0x14023d5b0 / 0x140a53970 |
| alive input (controller vt+0x40) | 0x1404135c0 | 0x1404134a0 (vtable 0x141812308) |

SM data offsets: vehicle ratios +8 (desired steer +0xcf8, drive +0xcfc, actual steer +0xce4, AI flag +0xcdc),
steering +0xc58, follower +0xc70, player pci +0x22c8 (relMove +0x22f8), rider handler +0x24b8, vrd +0x25d0 (R3).
Behavior, scheme, wheel and steering-params offsets are unchanged. Not ported by signature: 0x14023c010 (hand IK).

## Open items
* Whether the input layer applies a dead zone or response curve to LX before 0x1412e3fb0.
* Whether 0x140c2bb90 includes the current frame's bone-follower overrides (affects which grip pose to use).
* `prj_iGameOptionPreferredRidingView` enum value for first person; whether third-person riding forces a camera
  that bypasses vrd.
* Handbrake source (operator+0x10c) and the meaning of puppet state +0x658 values 2/3 beyond "operated".
* `wvsas_bUseHandlebar` reader (none found); `CWheeledVehicleSkeletonAdjuster` instantiation.
* Lab check: sign of H on the steering bone against the stick direction; that LS steering is proportional in
  the ATV, chopper and SM snowmobile.
