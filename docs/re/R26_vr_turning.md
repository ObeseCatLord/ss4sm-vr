# R26: native VR turning (anchor yaw) for the local VR player (Sam4.exe 616154, SamSM.exe 628492)

Static analysis only. Sources:
- Ghidra decompiles from a private scratch copy of `lab/ghidra_r19` (`tools/ghidra/DecompileList.java`). The decompiles
  are kept privately in `lab/r26/a1.c` and `a2.c`, and the address lists in `a1.txt` and `a2.txt`.
- capstone disassembly (`tools/disasm.py`), `callers.py`, `cvar_readers.py` (each hit re-decoded, §1.6),
  `field_refs.py`, `vtable_slot.py`, `meta_class.py`, `port_sites.py`, `sigscan.py`, `tools/port_symbols.py`.
- The existing lab log `lab/game/sm/Bin/x64/ss4smvr/ss4smvr.log` (SM, `native_vr_action=1`, MP client). It was read,
  not produced, for this note.

Nothing was run. No game file, cvar, user setting, save or `src/` file was touched. Addresses are Sam4.exe VAs unless
they are marked **SM**. "vrd" is `CPlayerVRData*` (player+0x25c8, SM +0x25d0). The vrd field offsets used below are
identical in SM: the rotation handler, the poll and anchor_turn decode to the same displacements.

Confidence tags: **[H]** read directly in code in both builds; **[M]** inferred from code, needs a runtime check;
**[L]** plausible, not established.

## 0. Answers

1. **Why the view does not turn: the stick never reaches the engine.** The engine's turn code reads cvars whose
   defaults would give snap turning with the right hand. It reads the stick from vrd's per-hand input table, not
   from the device:
   - It reads `vrd+0x14` (role 0 = right, stick x) and `vrd+0xe4` (role 1 = left).
   - The table is refreshed only by **`vrd_button_poll` 0x14043ac30** (SM 0x14043adb0), which reads device slots
     0xb8/0xd0.
   - That function has exactly two callers: `vrd_init` (once, at spawn) and the **`CPlayerBotEntity`** VR input path
     0x14048aca0. Nothing calls it per tick for a human player.
   - So `vrd+0x14` keeps the value from spawn, normally 0, and the mod's device button 2 is never read again.
   - The SM lab log agrees. The poll's only button reads (`…ae65`) appear in the same second as
     `CPlayerVRData … initialised`. From then on slot 0x0d0 runs exactly 2 calls per frame
     (596/298, 592/296, 572/286): the two back-button reads. A per-tick poll would add 20 or more per tick. [H]

   Chain:
   - The vrd tick **0x1404399c0** (the mod already hooks it as `vrd_tick`) runs each locomotion handler's step.
   - The rotation handler's step **0x14044d530** calls **`vr_rotation` 0x14044e880** (SM 0x14044e9c0).
   - That calls **`vr_anchor_turn` 0x1404425e0**(vrd, yaw rad). It adds the yaw to **vrd+0x674** and calls
     **0x140441e80**, which rotates the anchor **vrd+0x2c0** (P7) about the current head position and keeps it
     heading-only.
   - The camera is head world 0x14043cb50 = anchor ∘ device slot 0x80, so the view follows the anchor.

   Cvar meanings:
   - `vr_iRotationMethod`: **1 = smooth, 2 = snap**. Any other value (the default is 0) means smooth if trackpad
     locomotion is on, else snap. No value means "off".
   - `vr_iRotationController`: **2 = right hand (role 0)**, 1 = left (role 1), 3 = both (none while
     `vr_bStepMovement`), other values = no turning.

   Speeds:
   - Smooth: **9.00009 × `vr_fRotationSensitivity` × s rad/s**, where s = clamp((|x| − `vr_fContinuousMovementDeadzone`)
     / (1 − dz), 0, 1). With the defaults that is a maximum of 4.50 rad/s, about 258°/s.
   - Snap: **`vr_fRotationAngle`°** (45) when |x| ≥ `vr_fStrafeThreshold` (0.5), repeated every
     1/`vr_fRotationFrequency` s (0.5 s) while held.
   - Stick x > 0 gives a **negative** yaw delta.

   The .data defaults (table §1.5): method 0, trackpad locomotion 0, controller 2, deadzone 0.2, threshold 0.5,
   angle 45, sensitivity 0.5, frequency 2.0. With these defaults the engine snap-turns as soon as the right-hand
   stick value reaches it. [H]
2. **The anchor yaw is local only.**
   - Turning runs only for the local vrd. The rotation step needs `vrd+0x328`, which is 0 for a remote copy.
   - Neither vrd+0x674 nor the anchor travels on the network.
   - `ClientActionVR` sends the head **device-local** (`qvTrackerPlacement` = 0x14043d750 / the mod's slot 0x80
     head). It sends the controllers in **world** space (0x14043b920 = the client's turned anchor ∘ grip).
   - The receiver (0x140461a80 → 0x140470310 tail) computes `vLookDirEul = Euler(head world)` with **its own**
     anchor for that player. That anchor keeps the spawn heading.
   - Result: on the server, and on every machine that gets the replicated `en_qvNetTrackerPlacement`, the VR client's
     look, facing and head are off by the accumulated turn. Its hands and weapons are correct. Its own client
     predicts the turned look.
   - Fix (§3.5): in `hookedCa`, pre-rotate the sent head by vrd+0x674 about the up axis. That is the head expressed
     against the un-turned anchor, the same frame as the engine's MR anchor 0x14043af00. [H] for the data flow,
     [M] for the exact position term.
3. **The minimal safe intervention is three in-place patches inside the turn path. No cvar is written.**
   1. P1 `vr_rot_method_read` 0x14044e918 / SM 0x14044ea58: `8B 15 disp32`. Retarget the disp32 to a mod int.
      1 gives smooth turning, 2 gives snap. This is the only gameplay reader of `vr_iRotationMethod`.
   2. P2 `vr_rot_role` 0x14043ed10 / SM 0x14043ee60: `8B 05 disp32`. Retarget to a mod int = 2 (right hand). Its
      only other caller is the inert teleport handler.
   3. P3 `vr_rot_stick_r_load` 0x14044e99c / SM 0x14044eadc: `48 8B 43 10 F3 44 0F 10 40 14` (10 bytes) →
      `F3 44 0F 10 05 rel32` + `90`. This loads the role-0 stick x from a mod float instead of the stale table.
   4. Optional P4 `vr_comfort_mode_read` 0x1401dac25 / SM 0x1401da7f5: `8B 05 disp32`. Every turn sets the VR
      viewer's comfort-mask (vignette) trigger. This code has never run under the mod. Retarget it to a mod int (0
      off) until it has been lab-checked.

   - All four sites are unique in all four exes (§6). The patches need mod data within ±2 GB
     (`se4::patch::allocNear`, RWX).
   - The mod writes the right stick x into the float only with `native_turn` (and not while the wheel holds the
     stick), else 0.
   - Feeding device button 2 or calling the engine poll each tick is **not** enough or not safe (§3.3).

## 1. The engine turning path

### 1.1 Call chain (both builds identical in shape)

| step | Sam4 | SM | evidence |
|---|---|---|---|
| vrd tick (mod hook `vrd_tick`) | 0x1404399c0 | 0x140439b40 | a1.c 377; runs each handler in vrd+0x278 (count vrd+0x280) when `prj_bVRWeaponAdjustMode` (0x142285e50) == 0, temp weapon-adjust (0x142285e54) == 0, vrd+0x2ac == 0 and **(vrd+0x2bc & 0x18) == 0x18**. Handler vt+0x10 runs only when **vrd+0x328 ≠ 0 (local)**; vt+0x18 always |
| handlers created | vrd_init 0x14043e330 | 0x14043e480 | a1.c 2288: [0] = 0x58-byte object, ctor 0x14044a440, **vtable 0x141849c78 (rotation/step handler)**; [1] = 0xe8-byte object, ctor 0x14044a4a0, vtable 0x141849c28 (teleport handler, uses the teleport marker models) |
| movement bits | vrd_init | | bit 8 and bit 0x10 of vrd+0x2bc are both set from `CPlayerVRParams::pvp_bEnablePlayerMovementByDefault` (+0xac; meta rec 0x14183bb28, SM 0x14181f518). `PlayerVRParameters.rsc` (SS4, used by SM too, R9 §8) has it `TRUE`. No extracted Lua calls `Enable*PlayerMovement` |
| rotation handler step (vt+0x10) | 0x14044d530 | 0x14044d680 | a1.c 2987: returns if any handler's vt+0x30 is true (0x14043eef0, a pending timer); then **calls vr_rotation at 0x14044d54d** (SM 0x14044d69d); the rest is step movement only if `vr_bStepMovement` |
| **vr_rotation** (handler: +8 player, +0x10 vrd, +0x50 next snap time) | **0x14044e880** | **0x14044e9c0** | a1.c 1; disasm §1.3 |
| role enabled | `vr_rot_role` 0x14043ed10 | 0x14043ee60 | leaf (no .pdata); disasm |
| controller-type gate | 0x14044a780 | 0x14044a8a0 | a1.c 149 |
| **vr_anchor_turn** (vrd, xmm1 = yaw rad) | **0x1404425e0** | **0x1404426c0** | `vrd+0x674 += yaw; 0x140441e80(vrd, yaw); vrd+0x65c = 0`; only caller vr_rotation (0x14044ebbb / SM 0x14044ecfa) |
| anchor rebuild about the head | 0x140441e80 | 0x140441fe0 | a1.c 957: euler→quat(yaw,0,0) applied about the local head position (0x14043d750), anchor reduced to heading only (quat→Euler, P=B=0), written to vrd+0x2c0..+0x2dc (and the body-IK handler vrd+0x678); if \|yaw\| > 0.001 and the player is local and the viewport's VR viewer (+0x360) exists: **viewer+0x120 = 1** (0x1401de290, comfort-mask trigger, §3.4); vrd+0x660 = now |
| camera | 0x1404794a0 mode 0 → head world 0x14043cb50 | 0x14043cc90 | anchor (vrd+0x2c0; MR anchor 0x14043af00 for device+0x18 == 3) ∘ 0x14043d750 (local: device slot 0x80; remote: vrd+0x32c) |

The anchor yaw stays turned. In the movement-enabled branch of the anchor update 0x14043f180 (SM 0x14043f2d0, a1.c
1123), the anchor **position** follows the body and the anchor **rotation** is re-reduced to its own heading
(0x14043fdc7..). Only the movement-disabled branch copies the body heading. The other anchor-rotation writers are:
- the vrd ctor;
- vrd_init (spawn heading);
- the anchor reset 0x1404415d0, which places the head at a given placement. It is called from the look component
  when the look-reset counter changes (0x14049db43), from 0x140490010 and 0x140699885, and from 0x14043f010 only
  when the movement bits are clear. [H]

A side note on mode (a): with the movement bits set, no path was found from the desktop look (player+0x22c0) to the
anchor yaw. Pad turning should therefore turn the aim and body but not the head-world camera. If pad turning visibly
turns the view in the lab, that path exists and was missed. Check 7 in §5. [M]

### 1.2 The per-hand input table and why it is stale

`vrd_button_poll` 0x14043ac30 (SM 0x14043adb0; a1.c 1703, disasm identical apart from the prologue):

| field | meaning |
|---|---|
| vrd+0x328 | = 1 (local; also set by vrd_init from player vt+0x670) |
| vrd+8 + r·0xd0 | role r active = device **slot 0xb8**(r) (r = 0 right, 1 left, 2 aux) |
| vrd+0xc + r·0xd0 + i·4 | value of button i = device **slot 0xd0**(r, i, &flags) (xmm0) → **i = 2 stick x at vrd+0x14 / +0xe4**, i = 3 stick y at +0x18 / +0xe8 |
| vrd+0x34 + r·0xd0 + i·4 | flags (1 pressed, 2 held, 4 consumed, 8 released; consumed kept while held) |
| side effects | `VRCommandPressed` script event for each of the 8 command buttons vrd+0x28c..+0x2a8 that was pressed this frame and not consumed (SM log: indices 0 6 1 5 4 4 8); VR move 0x140442f80 → vrd+0x628/+0x634; trigger bits vrd+0x288; resets 0x142285e54 |

Callers (`callers.py`, both builds):
- 0x14043e552 in vrd_init (SM 0x14043e6a2);
- 0x14048ad14 in **0x14048aca0** (SM 0x14048b334 in 0x14048b2c0), reached only from 0x1404fc810 = slot +0x208 of
  vtable 0x1418b3258, whose ctor 0x1404dd6b0 sits next to `CPlayerBotEntity::UseKillBossCheat`. This is the VR test
  bot (SM 0x1404fcd10).

The live human path, controller vt+0x40 → ClientAction (R3 §4), never polls. So in SS4/SM every stick or button
consumer that reads the vrd table sees the spawn-time snapshot. That includes the rotation handler, the teleport
handler, VR move and `0x14043e9a0` button tests. [H]

Mapping to the mod's device (`src/se4/vr_device.cpp`):
- slot 0x0d0 button 2 = `sx` (stick x) and 3 = `sy` (0) per role;
- role 0 = right hand (`xr::input().hand(1)`);
- slot 0x0b8 = role active;
- slot 0x110 = device name: "WindowsMR" changes the turn's zero test from 0.001 to 0.1;
- slot 0x118 = per-role controller name, used by the gate 0x14044a780 (below);
- slot 0x210 = skeletal hand (only with "Knuckles").
The rotation reads none of these per tick except 0x110 and, when the stick is off zero, 0x118.

### 1.3 `vr_rotation` 0x14044e880 (disasm; SM 0x14044e9c0 same instruction stream, constants at shifted addresses)

1. Return (and clear the snap timer +0x50 = NONE `*0x1422566e0`, SM `*0x142235040`) in either case:
   - `0x14043efe0(vrd)`: a cached per-player object (vrd+0x6d8, class `*0x1422857d8`) has any live entry in its
     +0x1c8 array (class not identified, [L]: a grab or climb state);
   - neither role is enabled by `vr_rot_role`.
2. `trackpad = session(0x14024c680(player))+0x68 || vr_bTrackpadLocomotion`.
   `smooth = (vr_iRotationMethod ∈ {1,2}) ? (method == 1) : trackpad`, read at **0x14044e918**.
3. Device slot 0x110 name; `eps = (name == "WindowsMR") ? 0.1 : 0.001`.
4. For each enabled role r:
   - `a_r = vrd+0x14` (r = 0, loaded at **0x14044e9a0**) or `vrd+0xe4` (r = 1, at 0x14044e9eb);
   - the role counts if `|a_r| > eps` and `0x14044a780(ctx, r)` == 0.
   - The gate 0x14044a780 reads the device slot 0x118 name for r:
     - "Rift" or any unknown name: pass.
     - **"Vive"**: blocked unless button 4 is held in the (stale) table, given `vr_bTrackpadClickToMove` or a snap
       method.
     - "WindowsMR": blocked only when `vr_bWindowsMRJoystickMovement` == 0 (default 1) and click-to-move.
     - "Knuckles" with slot 0x210 ≠ 0: blocked unless button 4 is held, given click-to-move or a smooth
       method.
   - `x = Σ a_r`.
5. **Smooth** (`smooth` ≠ 0):
   - `s = sign(x)·clamp((|x| − dz)/(1 − dz), 0, 1)`, where dz = `vr_fContinuousMovementDeadzone`, read at
     0x14044ea7c/84;
   - stop if `|s| ≤ eps`;
   - `yaw = −(step_time(player) × 9.00009 × vr_fRotationSensitivity × s)`. step_time is 0x140ba7860, the world
     step in seconds (R12). The constant is at 0x141849da4 (bits 0x4110005F; SM 0x14182d784). Sensitivity is read
     at 0x14044eac8; SM multiplies in a different order (0x14044ec0f).
6. **Snap** (`smooth` == 0):
   - if `|x| < vr_fStrafeThreshold`, set timer = NONE and stop;
   - else, if the timer is NONE or has expired (`now` = 0x140ba78c0, 32.32 fixed seconds), set
     `timer = now + 2³²/vr_fRotationFrequency` and `yaw = vr_fRotationAngle × (−π) × (1/180) × sign(x)`.
7. Call `vr_anchor_turn(vrd, yaw)`.

`vr_rot_role(vrd, role)` 0x14043ed10:
- `c = vr_iRotationController`;
- c == 3 and !`vr_bStepMovement` → role ≤ 1;
- c == 2 → role == 0;
- c == 1 → role == 1;
- else 0.
Callers: vr_rotation (×2) and the teleport handler step 0x14044d6c0 (×2) only. [H]

### 1.4 Device-slot view of the turn

| engine wants | from | the mod today |
|---|---|---|
| right stick x, per tick | vrd+0x14 ← poll ← slot 0xd0(0, 2) | slot 0xd0 reports `sx` with `native_turn`, but nothing polls it per tick |
| left stick x | vrd+0xe4 ← slot 0xd0(1, 2) | same |
| per-hand type | slot 0x118 per role | "Rift" for Index/Touch/Frame (passes the gate), "WindowsMR" (passes with the default joystick cvar), **"Vive" for unknown kinds (blocked)** |
| device type | slot 0x110 | "Vive"/"Rift"/"WindowsMR"; only the zero threshold depends on it |

### 1.5 Cvars and their compiled defaults

All have flags 0x10006 (persistent user settings) or 0x10002. "file init" is the value in the image. Variables past
the raw end of .data (Sam4 raw end 0x1422733ff, SM 0x1422505ff) are zero-initialised. The only code writer of
`vr_iRotationMethod` and `vr_bTrackpadLocomotion` is the bot VR testing preset (0x140509ced, gated by
`bot_vrtmVRTestingMode`). The options menu `CMSVRControlsOptions` (0x1408efcaf) and the user's settings file can
change any of these values at runtime. The settings file was not inspected.

| cvar | type | Sam4 VA | SM VA | file init | role in the turn |
|---|---|---|---|---|---|
| `vr_iRotationMethod` | INDEX | 0x1422862a8 | 0x1422633f8 | **0** (zero-init) | 1 smooth, 2 snap, else trackpad ? smooth : snap |
| `vr_bTrackpadLocomotion` | BOOL | 0x1422862a0 | 0x1422633f0 | **0** (zero-init) | only for method ∉ {1,2} |
| `vr_iRotationController` | INDEX | 0x142248730 | 0x1422292e8 | **2** | turning hand (2 = right) |
| `vr_fContinuousMovementDeadzone` | FLOAT | 0x14224872c | 0x1422292e4 | 0.2 | smooth deadzone |
| `vr_fStrafeThreshold` | FLOAT | 0x142248728 | 0x1422292e0 | 0.5 | snap threshold |
| `vr_fRotationAngle` | FLOAT | 0x142248738 | 0x1422292f0 | 45 | snap angle (deg) |
| `vr_fRotationSensitivity` | FLOAT | 0x14224873c | 0x1422292f4 | 0.5 | smooth speed factor |
| `vr_fRotationFrequency` | FLOAT | 0x142248740 | 0x1422292f8 | 2.0 | snaps per second while held |
| `vr_bStepMovement` | BOOL | 0x142286298 | 0x1422633e8 | 0 (zero-init) | controller 3 → none; step movement after the turn |
| `vr_bAllowMovementOnRotationController` | BOOL | 0x142248734 | 0x1422292ec | 1 | movement stick reader 0x140444a20 only |
| `vr_bTrackpadClickToMove` | BOOL | 0x142248724 | 0x1422292dc | 1 | gate 0x14044a780 (Vive/WMR) |
| `vr_bWindowsMRJoystickMovement` | BOOL | 0x14224cb34 | 0x14222b8e4 | 1 | gate 0x14044a780 (WMR) |
| `vr_bEnableComfortLocomotion` | INDEX | 0x142247d98 | 0x1422289f8 | 1 | comfort mask on turns (§3.4) |
| `vr_fComfortLocomotionFadeDuration` | FLOAT | 0x142247da0 | 0x142228a00 | 0.2 | mask fade time |

### 1.6 Corrections to the brief

`cvar_readers.py` accepts a disp32 followed by up to 4 immediate bytes. A read of the 4-byte neighbour below a cvar is
therefore also listed under that cvar. After decoding each hit:
- **0x14044ea7c/84 and 0x140444d15/20 read `vr_fContinuousMovementDeadzone` (0x14224872c), not
  `vr_iRotationController`.** The real readers of `vr_iRotationController` are 0x14043ed10 (`vr_rot_role`) and
  0x140444b9d (movement stick reader 0x140444a20); SM 0x14043ee60 and 0x140444ca7.
- `vr_iRotationMethod` has three code references, not four. 0x14107daa3 is a false hit. The three are the read
  0x14044e918, the bot preset write 0x140509ced and the options-menu read 0x1408efcaf (SM 0x14044ea58, 0x14050a1dd,
  0x1408f006f).

## 2. Local presentation or networked? (Q2)

| item | owning client | server and other peers |
|---|---|---|
| turn input / handler | runs (vrd+0x328 = 1) | never runs for a remote vrd (vrd+0x328 = 0) |
| anchor vrd+0x2c0, yaw vrd+0x674 | turned | spawn heading (vrd_init) plus resets; not replicated (no RPC argument, no `en_*` member) |
| `ClientActionVR qvTrackerPlacement` | device-local head (native sender 0x14048b730 → 0x14043d750; mod: `vrdev::headPlacement` = slot 0x80) | stored at vrd+0x32c, used as the remote local head by 0x14043d750 |
| `qvControllerR/L` | world = turned anchor ∘ grip (0x14043b920) | stored at vrd+0x348/+0x364 and used as world → **hands and weapons correct** |
| `vLookDirEul` | not sent; the stub tail 0x140470310 computes Euler(head world) with the **local** anchor → turned look (client prediction) | the receiver 0x140461a80 → same tail → Euler(**server anchor** ∘ device-local head) → **look misses the turn** → player+0x22c0 and facing; movement direction probably too [M] |
| `en_qvNetTrackerPlacement` (+0x2618) | – | the server's 0x14048a770 copies its 0x14043d750 (= what the RPC wrote) to every peer; peers compose with their own un-turned anchor |

The anchor yaw is therefore **client-local presentation**. Without a fix, a turned VR client is seen facing, and is
moved by the server, along `spawn heading + HMD yaw` instead of `spawn heading + turn + HMD yaw`. The mod's own pose
channel (net.cpp: head and hands relative to the renderable, from head world) is unaffected.

**Fix: send the head relative to the un-turned anchor.**
- Head world = anchor ∘ head, and the turned anchor = un-turned anchor rotated by `t` = vrd+0x674 about the head
  (0x140441e80; R14 §3.1 reads the MR anchor 0x14043af00 as exactly this inverse).
- So the head expressed against the un-turned anchor is `q' = Y(t) ⊗ q_head`, `p' = p_head`.
  - Y(t) is the engine's yaw quaternion for heading t: `euler_quat` (H, P = 0, B = 0), the same call 0x140441e80
    makes.
  - Since both anchors are heading-only, `R_srv · Y(t) = R_client` exactly.
- Sending `(q', p')` as `qvTrackerPlacement` makes the server's look, and the replicated tracker on every peer,
  match the client.
- On the sending client the argument only lands in vrd+0x32c, which the local branch of 0x14043d750 ignores (it reads
  slot 0x80). The client's own look and view are unchanged.

Limits:
- [M] The position term is exact while the head stays where it was when it turned. It drifts by `(Y−I)(p − h)` as
  the head moves in the play space after a turn. It only affects the remote head position, not the look.
- [M] Anchor resets (look-reset counter, teleports) run on both sides and are assumed to happen alike.
- On a VR **host**, `en_qvNetTrackerPlacement` is filled from the host's own 0x14043d750 (device-local). Peers'
  copies of the host's head therefore miss the host's turn. The host's look is computed locally and is correct.
  Fixing the head would need a hook on 0x14048a770 (not traced further).

## 3. Minimal intervention (Q3)

### 3.1 Patches

All four are rip-relative retargets or rewrites of one instruction pair. Each lies wholly inside the turn path or the
comfort pass. The original bytes are checked with `se4::patch::writeCode` before writing. The data targets live in one
small block from `se4::patch::allocNear(site)` (RWX, within rel32 reach of every site).

| id | symbol (`manual:`) | Sam4 site | SM site | original bytes | new bytes | mod value |
|---|---|---|---|---|---|---|
| P1 | `vr_rot_method_read` | 0x14044e918 | 0x14044ea58 | `8B 15` + disp32 → `vr_iRotationMethod` (Sam4 `8A 79 E3 01`, SM `9A 49 E1 01`) | `8B 15` + rel32(&g_rotMethod, site+6) | int: **1** smooth (default), 2 snap (option) |
| P2 | `vr_rot_role` | 0x14043ed10 | 0x14043ee60 | `8B 05` + disp32 → `vr_iRotationController` | `8B 05` + rel32(&g_rotRole, site+6) | int **2** (right hand = role 0) |
| P3 | `vr_rot_stick_r_load` | 0x14044e99c | 0x14044eadc | `48 8B 43 10 F3 44 0F 10 40 14` (`mov rax,[rbx+0x10]; movss xmm8,[rax+0x14]`) | `F3 44 0F 10 05` + rel32(&g_turnAxis, site+9) + `90` | float: right stick x or 0 |
| P4 (optional) | `vr_comfort_mode_read` | 0x1401dac25 | 0x1401da7f5 | `8B 05` + disp32 → `vr_bEnableComfortLocomotion` | `8B 05` + rel32(&g_comfortMode, site+6) | int **0** until checked; 1 to keep the native turn vignette |

Why each patch is safe:
- **P3:** rax is dead after the pair (the next instruction is `mov eax, edi`). No branch targets
  0x14044e9a0..0x14044e9a5 (or the SM equivalent). Capstone decodes the new bytes as `movss xmm8, dword ptr
  [rip+rel32]; nop`.
- **P2:** it also changes the role answer for the teleport handler 0x14044d6c0. That handler reads only the stale
  table (vrd+0x14/+0x18/+0xe4/+0xe8, button flags at vrd+0x2a0), so it stays inert. P2 differs from stock only if the
  user's saved `vr_iRotationController` ≠ 2. Without P2, a saved value of 0 or of 3 with step movement disables
  turning.
- **P1:** the menu reader 0x1408efcaf and the bot preset keep the real cvar. Nothing is saved.
- `vr_iRotationController`'s second reader (0x140444b9d, movement stick reader) keeps the real cvar. It only feeds
  stale-table consumers.
- The role-1 load `vr_rot_stick_l_load` (0x14044e9e7, 12 bytes `48 8B 43 10 F3 0F 10 B0 E4 00 00 00`) is left alone.
  With P2 = 2, role 1 is never evaluated.

What still comes from user cvars (read only; the mod may read them to scale its axis):
- dz, sensitivity, threshold, angle and frequency;
- `vr_bTrackpadClickToMove` and `vr_bWindowsMRJoystickMovement` (only for "Vive"/"WindowsMR" hands).

Optional redirects of the sensitivity and angle reads are possible, but they were not added as symbols because their
instruction forms differ between the builds:
- sensitivity: Sam4 `movss xmm1,[rip]` at 0x14044eac8, SM `mulss xmm0,[rip]` at 0x14044ec0f, disp at +4 in both;
- angle: `movss xmm1,[rip]` at 0x14044eb9b, SM 0x14044ecda.

### 3.2 Speed without writing cvars

Smooth turning reaches `ω_max = 9.00009 × vr_fRotationSensitivity` rad/s (4.50 rad/s ≈ 258°/s at the default 0.5).
For a mod speed `ω` (option, e.g. `turn_speed` in deg/s):
1. Let `r = min(1, ω/ω_max)`.
2. Apply the mod's own deadzone and curve `c(|x|) ∈ [0,1]`.
3. Write `g_turnAxis = sign(x) · (dz + (1 − dz) · r · c(|x|))`, with dz = `vr_fContinuousMovementDeadzone` read from
   the cvar.
4. Write 0 when `c` is 0, so that the engine's `|x| ≤ 0.001` early-out applies.

Snap needs no scaling: write the raw x. The engine fires on `|x| ≥ vr_fStrafeThreshold` and re-arms when the stick
returns below it.

### 3.3 Rejected alternatives

- **Device button 2 alone**: it is already fed and never read per tick (§1.2).
- **Call `vrd_button_poll` per tick** (e.g. from the vrd tick hook): this refreshes everything natively but also:
  - fires `VRCommandPressed` for the 8 command buttons (in native mode the device reports all buttons; `vr_device.cpp`
    notes that the engine's VR code acted on grip and pad at spawn and opened Netricsa);
  - runs VR move 0x140442f80;
  - resets the temporary weapon-adjust flag 0x142285e54 every tick;
  - wakes the teleport handler, which would then see live sticks and buttons with `vr_bTrackpadLocomotion` = 0
    (teleport locomotion).
  It would need the device to report only button 2 during the poll. That is more state to manage than P3.
- **Write vrd+0x14 from the vrd tick hook**: no code patch, but the teleport handler 0x14044d6c0 reads vrd+0x14
  directly against `vr_fStrafeThreshold` (a1.c 2662). So does the "is moving" test 0x14043eaf0 via 0x140444a20, which
  affects anchor-update branches. P3 keeps the value private to the turn.
- **Write the cvars**: forbidden (flags 0x10006 are saved with the user's settings).

### 3.4 Side effect of any working turn: the comfort mask

0x140441e80 sets VR viewer+0x120 on every turn of more than 0.001 rad. The VR view 0x1401dcbe0 calls 0x1401dabf0 at
0x1401dd2aa (SM function 0x1401da7c0).
- With `vr_bEnableComfortLocomotion` == 1 (default), a fade value at viewer+0x130 rises over
  `vr_fComfortLocomotionFadeDuration` (0.2 s) while turns keep coming, and falls when they stop. Mode 2 means always
  on, 0 means off.
- While the fade value is > 0.001, the pass draws a vignette mask (`vr_fComfortLocomotionMaskRadius`,
  `…MaskGradientExponent`). It fetches eye targets through device slot 0x28 (eyes 1 and 2), copies them with
  `gfx_copy` into the viewer's holders +0x40/+0x48, and re-enters 0x1401dd800.
- Smooth turning therefore holds the mask on for the whole turn.
- This pass has never run under the mod (no turn ever happened), and it touches the mod's eye targets. P4 = 0 keeps it
  off until a lab run shows it works with single-pass stereo and the upscaler. [H] for the trigger and cvar, [M] for
  the visual behaviour.

### 3.5 MP: head sent in ClientActionVR

See §2. This applies only when `nativeVrAction()` is true, which is always the case for an MP client (`player_vr.cpp`).

## 4. Recipe for `src/se4/player_vr.cpp` / `src/se4/vr_device.cpp` (not applied)

```
// player_vr::install (localVr), after the existing guards; all four sites from se4::gen (manual symbols above)
struct TurnData { int32_t method, role, comfort; float axis; };
auto* site1 = b.at<uint8_t>(se4::gen::vr_rot_method_read);   // 8B 15 d32
auto* site2 = b.at<uint8_t>(se4::gen::vr_rot_role);          // 8B 05 d32
auto* site3 = b.at<uint8_t>(se4::gen::vr_rot_stick_r_load);  // 48 8B 43 10 F3 44 0F 10 40 14
auto* site4 = b.at<uint8_t>(se4::gen::vr_comfort_mode_read); // 8B 05 d32 (optional)
g_turn = reinterpret_cast<TurnData*>(se4::patch::allocNear(site3, sizeof(TurnData)));
*g_turn = {options::get("snap_turn",0) ? 2 : 1, 2, options::get("turn_vignette",0), 0.f};
// P1/P2/P4: keep opcode bytes, replace disp32 (expect = current 6 bytes, checked by writeCode)
retargetRip(site1, 2, 6, &g_turn->method); retargetRip(site2, 2, 6, &g_turn->role);
if (site4) retargetRip(site4, 2, 6, &g_turn->comfort);
// P3
uint8_t p3[10] = {0xF3,0x44,0x0F,0x10,0x05, 0,0,0,0, 0x90}; put32(p3+5, rel32(&g_turn->axis, site3 + 9));
writeCode(site3, kStickLoadExpect /*48 8B 43 10 F3 44 0F 10 40 14*/, p3, 10);
VRLOG("turn: native turn patched (method %d, role 2, comfort %d) at %p %p %p %p", ...);
```

Notes:
- Install the patches whenever the local player can be VR. Leave `g_turn->axis` = 0 unless `nativeTurn()`, so stock
  behaviour is unchanged otherwise.
- **Axis feed.** Write `g_turn->axis` each frame in `vrdev::updateFromInput()` or at the start of `hookedVrdTick` for
  the local vrd:
  - the right hand's `stick.x`, mapped per §3.2 for smooth, raw for snap;
  - only when `player_vr::nativeTurn()` and the wheel does not hold the stick (mirror `gamepad.cpp:173`) and no menu
    is up;
  - else 0.
  A 4-byte aligned float store is atomic. The sim thread reads it once per tick.
- `vr_device.cpp updateFromInput`: button 2 no longer matters (only read at spawn). Report `sx = 0` to keep the spawn
  snapshot neutral, and update the comment at line 658, which wrongly says that button 2 feeds 0x14044e880.
- **Controller names.** Per-hand slot 0x118 must not say "Vive" for thumbstick controllers, or the gate 0x14044a780
  blocks the turn. The current `nameForKind` returns "Vive" for `ControllerKind::Unknown`. Report "Rift" for unknown
  kinds that have a thumbstick.
- **MP head (§2).** In `hookedCa`, before `realCavr`:
  ```
  float t = *(float*)((uint8_t*)vrd + 0x674);          // same offset SS4/SM (anchor_turn)
  quat Y = euler_quat(t, 0, 0);                        // engine euler_quat symbol (R7)
  head.q = Y * head.q;   // anchor-space pre-multiply; head.p unchanged
  ```
  Log once per second: `t`, client look H vs the head world H.

## 5. Runtime log checks (lab first, per the lab notes; SS4/SM lab runs need the user's go-ahead)

1. Install line: `turn: native turn patched …` with the four Sam4 or SM addresses from §6, each with its expected
   original bytes found. A byte mismatch must leave the site untouched and log it.
2. With `native_vr_action=1` in a level, the right stick held right logs (1 Hz) `turn: axis 0.xx yaw(vrd+0x674)
   -y.yy`:
   - smooth: yaw changes about `ω·Δt`;
   - snap: yaw steps by −0.785 every 0.5 s.
   The view turns, and hands and weapons stay on the controllers.
3. Stick released: the yaw is constant and the comfort mask stays off with P4 = 0. With P4 = 1, look for a vignette
   during turns and no corruption of the eye images.
4. Device slot 0x110 already runs about once per sim tick: the rotation handler's name query, which shows the handler
   is reached today. Slot 0x118 is the real signal. It is called per hand only when the stick value is off zero
   (gate 0x14044a780). In the SM log it stayed at 2 calls in total (both at spawn), as expected with a stale 0. After
   P3 it must rise while the stick is held. Slot 0x0d0 stays at about 2 per frame (no poll).
5. `native_turn=0`: the axis stays 0, vrd+0x674 stays 0, and pad turning behaves as before.
6. MP (client and server lab):
   - With the head fix, the server's `player+0x22c0` heading for the VR client (log via `mp_log`) matches the
     client's after a 90° turn.
   - Without the fix it differs by 90°.
   - Hands stay correct either way.
7. Mode (a) check for §1.1: with `native_turn=0`, does the pad's right stick rotate the head-world view, or only the
   aim and body? Log vrd+0x2c0 heading and player+0x22c0 heading.
8. After a clean exit, the user's settings contain no new or changed `vr_iRotation*`, `vr_bEnableComfortLocomotion`
   or `vr_fRotation*` values. Check by diff against a copy taken beforehand, with the user's go-ahead.

## 6. Address table (Sam4 616154 / SamSM 628492)

New in `docs/re/builds/signatures.json`:
- `vr_rotation`, `vr_rot_method_read`, `vr_rot_role`, `vr_rot_stick_r_load`, `vr_rot_stick_l_load`,
  `vr_anchor_turn`, `vr_comfort_mode_read`: Sam4 signature;
- `vrd_button_poll`: Sam4 signature plus an `alt` from SM 0x14043adb0, because the prologues differ.

`tools/port_symbols.py` found each one unique in Sam4, Sam4_Unrestricted, SamSM and SamSM_Unrestricted. It printed no
FAIL line for Sam4 or SamSM. The FAIL lines it printed are older Unrestricted-only entries.

`tools/symbols.txt` gained those eight names plus `var:vr_fRotationSensitivity` and
`var:vr_fContinuousMovementDeadzone`. `src/se4/symbols_gen.hpp` was not regenerated.

| name | Sam4 | SamSM | note |
|---|---|---|---|
| `vr_rotation` | 0x14044e880 | 0x14044e9c0 | sig 64 |
| `vr_rot_method_read` (`8B 15` d32) | 0x14044e918 | 0x14044ea58 | sig 24; decodes to the cvar in both |
| `vr_rot_role` (`8B 05` d32 at fn start) | 0x14043ed10 | 0x14043ee60 | sig 24 |
| `vr_rot_stick_r_load` (10 bytes) | 0x14044e99c | 0x14044eadc | sig 24; bytes identical |
| `vr_rot_stick_l_load` (12 bytes) | 0x14044e9e7 | 0x14044eb27 | sig 24; bytes identical |
| `vr_anchor_turn` | 0x1404425e0 | 0x1404426c0 | sig 24 |
| anchor rebuild | 0x140441e80 | 0x140441fe0 | call in anchor_turn |
| `vrd_button_poll` | 0x14043ac30 | 0x14043adb0 | sig 64 + alt |
| `vr_comfort_mode_read` (`8B 05` d32) | 0x1401dac25 | 0x1401da7f5 | sig 24; comfort pass 0x1401dabf0 / 0x1401da7c0 |
| rotation handler step (calls vr_rotation at) | 0x14044d530 (0x14044d54d) | 0x14044d680 (0x14044d69d) | port_sites |
| vrd tick (existing `vrd_tick`) | 0x1404399c0 | 0x140439b40 | |
| rotation gate / handler-busy / controller-type gate | 0x14043efe0 / 0x14043eef0 / 0x14044a780 | 0x14043f130 / 0x14043f040 / 0x14044a8a0 | port_sites |
| poll callers: vrd_init / bot VR input / bot vt+0x208 | 0x14043e552 / 0x14048ad14 (fn 0x14048aca0) / 0x1404fc810 | 0x14043e6a2 / 0x14048b334 (fn 0x14048b2c0) / 0x1404fcd10 | callers.py |
| ClientActionVR stub / receiver | 0x140470310 / 0x140461a80 | 0x140470940 / 0x140463910 | port_sites |
| head world / local head | 0x14043cb50 / 0x14043d750 | 0x14043cc90 / 0x14043d890 | port_sites |
| step time / world now | 0x140ba7860 / 0x140ba78c0 | 0x140ba7f30 / 0x140ba7f90 | calls in vr_rotation |
| constants: 9.00009 / −π / 1/180 | 0x141849da4 / 0x1415e5cec / 0x14158bc00 | 0x14182d784 / 0x1415caa1c / 0x14156dc60 | values read from both images |
| rotation / teleport handler vtables | 0x141849c78 / 0x141849c28 | not ported | ctors 0x14044a440 / 0x14044a4a0 |

Confidence:
- **[H]:**
  - the call chain;
  - the stale per-hand table (callers, code and the lab call counts);
  - the cvar semantics and compiled defaults;
  - the smooth and snap math;
  - that the anchor yaw is not networked and that the receiver derives the look from its own anchor;
  - the patch sites, bytes and their uniqueness;
  - the comfort trigger.
- **[M]:**
  - the turn direction (x > 0 gives a negative yaw: expected to be a right turn);
  - the head pre-rotation's position term and that anchor resets match on both sides;
  - that pad turning does not move the anchor (check 7);
  - the look of the comfort pass under the mod's renderer.
- **Not determined:**
  - the class behind the rotation gate 0x14043efe0 (vrd+0x6d8);
  - what the server does with a look that differs from the client's position (movement correction strength);
  - the host-side replicated head (§2).
