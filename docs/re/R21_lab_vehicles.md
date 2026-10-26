# Gate R21: boarding a vehicle in the lab (Sam4.exe 616154, SamSM.exe 628492)

Static analysis only. Sources:
- a private scratch copy of `lab/ghidra_r19` (`tools/ghidra/DecompileList.java`);
- `disasm.py`, `callers.py`, `vcall_sites.py`, `vtable_slot.py`, `field_refs.py`, `meta_offset.py`, `meta_enum.py`,
  `meta_class.py`, `port_list.py` and the harvest JSONs;
- the lab's own logs (`lab/game/ss4/Log/Sam4.All.log`, `Bin/x64/ss4smvr/ss4smvr.log`, read only);
- the private Lua dump (`lab/lua_all`) and the private `.ep` copies in `lab/r7/content`;
- a read-only zip scan of the official `.gro` archives (nothing extracted, nothing written).

Nothing was run. No game file, save or `src/` file was touched. Addresses are Sam4.exe VAs unless marked **SM**.

Confidence tags (as in R17/R20):
- **[D]**: read from a decompile or disassembly.
- **[M]**: from reflection, harvest or content.
- **[I]**: inference that needs a runtime check.
- **[L]**: read from the lab's own logs.

## 0. Answers

1. **`MountRide` did board, and the game then crashed. The crash is the real blocker.** **[L][D]**
   - Every lab run that called `player:MountRide(v, "Default")` on vehicle 1 (Harvester) or on vehicle 3 died
     1–2 s later with `ACCESS_VIOLATION` at **0x14043AAF5**. The logs list the runs at 22:47, 22:56 and 23:00. A
     22:51 run also crashed there, but its chunk text is unknown.
   - The stack is `0x14043aae0 ← 0x14044d6c0 ← 0x1404399c0` (the vrd tick).
   - RCX is the player (`0x7bf095f0`, the same value as in `player_vr: CPlayerVRData … initialised for player
     000000007bf095f0`). RBX = `*(player+0x1d0)` = **NULL**. `+0x1d0` is `CPuppetEntity::en_pbo` (the physics body).
   - A rider has no `en_pbo`, and the native **VR locomotion handlers** dereference it without a check (section 3).
   - The only attempt that "did not board" was on vehicle 2 (`GetRide()` was still nil after 2 s, with no crash). That
     vehicle was probably not mountable (section 1.3).
   - `EnterIntoRide` is a **stub in the final exe**: it reads its arguments and does nothing (section 1.2).
2. **Why the second `Wait(Delay(n))` "never returned".** **[L]**
   - In every run with a second `Wait`, the process crashed (above) before that delay ran out.
   - Example, 23:00:43: `try(2)`; the first `Wait(Delay(2))` resumed at 23:00:45 (`ride true nil`); `try(3)`; then
     the second `Wait(Delay(2))` and the crash at 23:00:47.
   - Nothing in the R20 scheduler path limits a chunk to one resume. Re-test after the crash is fixed. If a
     non-crashing run still hangs on a second `Wait`, R20 §6 item 2 (where the lsi step is called) is the next place
     to look. **[I]**
3. **Most reliable lab boarding (Lua bridge).**
   1. Turn the native VR locomotion off: `player:GetVRHandler():EnableGamePlayerMovement(false)`.
   2. Then call `player:MountRide(v, v:GetSeatName(0))` on a vehicle where `v:IsAlive()` is true and that has no
      rider.
   - `MountRide` skips every gameplay gate: lock, usage flag, distance and the PlayerRideSelection script.
   - The exact chunk is in section 5.
   - A native alternative on the simulation thread is in section 4.
4. **Seat name.** The seat *name* of every SS4 vehicle is **`Default`**. `DriverSeat` is the seat *attachment* (this
   corrects R7 §4). So `"Default"` was correct. `GetSeatName(0)`, or `""` (which MountRide maps to seat 0), avoids the
   question. **[M][D]**
5. **ATV levels.**
   - SS4: **only `01_PB/06b_Countryside.wld`** references `ATV_01.ep`. Its script runs `atvSpawner:SpawnOne()` at
     script start (one ATV in single player). A voice line places it "after the quarry", so it is not at the player
     start; teleport to it.
   - `06_Countryside` has the Harvester (usage disabled at start), the Chopper (spawned at start by
     `chopperSpawner:SpawnOne()`) and a Tractor.
   - SM: `01_SM/02_Siberia.wld` has `SpecialATV*`, `Snowmobile_v2` and `Tractor`.

## 1. How a player normally enters a vehicle

### 1.1 The use path [D]
`CPlayerActorPuppetEntity::UseEntity(entity)` (script glue **0x14045b7f0**, SM 0x14045d300) calls
**`use_entity(player, handle)` 0x14049bc80** (SM 0x14049c270, from the call order in the SM glue). The native `plcmdUse`
path and the VR hand-use path reach the same function: it has 14 direct callers, among them 0x140406705,
0x14041106d and 0x140442f17.

```text
use_entity(player, h):                                   0x14049bc80
  t = handle(h)
  if server/local:                                       (0x140ba6e30 == 0)
     if (t->vt[0x438]() & 1):                            usability, 0x1402497e0
        player+0x2038 = h
        t->vt[0x448](t, player)                          "used by"
        signal t.Used to scripts                         (the level scripts wait on vehicle.Used)
  else: send the RPC (descriptor 0x14231a660)
```

**Vehicle usability** (`CVehicleEntity` vt+0x438 = **0x14075adc0**, SM 0x14075a690):
- It returns 0 (not usable) unless `en_pnfNetFlags` (+0x7a4) has **PNF_ENABLED_TO_USE (4)**.
  - `CPuppetEntity::EnableUsage` 0x14028c270 sets that flag (SM 0x140281190).
  - `DisableUsage` 0x14028c430 clears it (SM 0x140281350).
- It returns 8 (bit 0 clear; prompt "The vehicle is locked", 0x14075ae80) if `en_ulVehicleFlags` (+0xc80, SM +0xc88)
  has **VF_LOCKED (2)** and either:
  - the key aggregator handle at **+0xd40** is null, or
  - `0x1406c43a0(agg)` (`(agg+0x98 & agg+0xa4) != 0`) is true.

  The aggregator is set by `CVehicleEntity::SetKeyAggregator` 0x140755b70.
  `Lock` 0x140755800, `Unlock` 0x140755840 and `IsLocked` 0x140755880 (SM 0x1407550b0 / 0x1407550f0 / 0x140755130)
  are plain bit operations on +0xc80.
- It returns 0 if `en_eaRiders` (+0x2a8) already holds a live rider.
- Otherwise it returns 9 (usable).

**Vehicle "used by"** (vt+0x448 = **0x14075d3a0**, SM 0x14075cc30):
- If the vehicle is locked: it returns while the aggregator is missing or not satisfied, and otherwise clears
  VF_LOCKED.
- Then it calls the puppet handler **0x1402b8f80** (SM 0x1402b8dd0), which does three things:
  1. **`can_ride(ride, user)` = vt+0x760 → 0x1402ad020** (SM 0x1402ace40) must pass:
     - the rider count must be below the seat count (vt+0x768);
     - then it is true if `pp_bRidingAlwaysAllowed` (params +0x610) is set, or if `0x140aa7980(world) == 0`;
     - otherwise, for each seat without `SF_PLAYER_CANT_SEAT` (0x10) whose distance check
       `|ride − user| < seat+0xac` passes, it calls the world Lua function `PlayerRideSelection(user, ride)`.
       `+0xac` is beyond the reflected `CPuppetSeatData` members, so it is taken to be a use radius **[I]**.
  2. vt+0x878 returns 1 for vehicles. Then the seat is chosen by **vt+0x788 → 0x1402a2ba0** (SM 0x1402a2960):
     - a single-seat ride always gets seat 0;
     - with more seats it scores each free seat by distance to the user's view position (≤ params +0x254) and by
       facing.
  3. **`mount(user, ride, seatName)` 0x1402ae7c0** (SM 0x1402ae5e0).

### 1.2 Script entry points [D]

| function | glue | arguments | effect |
|---|---|---|---|
| `CPuppetEntity::MountRide` | **0x14028dc20** (SM 0x140282b40) | `(ride, seatName : string)`; arg 2 is read as an IDENT and must be a string | Null ride → `"Ride provided to meta macro EnterRide() is NULL"`. Seat `""` → seat 0's `psd_idSeatName` (vt+0x770(0) +8). Then `mount(self, ride, seat)`. **No lock, usage, distance or rider-count check.** |
| `CLeggedPuppetEntity::EnterIntoRide` | 0x14026f1f0 (SM 0x14026f740) | `(ride, seat : string, 3 more scalar args)` | **Does nothing in the final exe.** It checks that the ride is alive, looks up the seat (vt+0x770 / vt+0x780) and returns. Its error came from passing no string as argument 2. |
| `CPlayerActorPuppetEntity::UseEntity` | 0x14045b7f0 (SM 0x14045d300) | `(entity)` | The full gameplay path in 1.1. |
| `CPuppetEntity::GetSeatName(i)` | 0x1402868f0 (SM 0x14028a5b0) | int index | `psd_idSeatName` of seat *i*, or `""`. |
| `GetRide` / `GetOperator` / `GetCountOfRidersInRide` / `IsAlive` | 0x14028b090 / 0x14028dda0 / 0x14028afa0 / 0x14028a350 | | +0x75c / +0x428 / +0x2a8 / +0x7a6 bit 0 (PNF_ALIVE) |
| `CPuppetEntity::EnableUsage` / `DisableUsage` | 0x14028c270 / 0x14028c430 | | set / clear PNF_ENABLED_TO_USE |
| `CVehicleEntity::Lock` / `Unlock` / `IsLocked` | 0x140755800 / 0x140755840 / 0x140755880 | | VF_LOCKED in +0xc80 |
| `CVehicleEntity::TurnOn` / `TurnOff` | 0x140286980 / 0x1402869c0 → vt+0x808 0x14075e840 / vt+0x810 0x14075e730 | | engine on/off (+0xcd8), behaviors and propulsion |
| `CPlayerActorPuppetEntity::ExitRide` | 0x140461340 → 0x140474fa0 | | Refused if the seat has `SF_PLAYER_CANT_EXIT` (0x80). Sends an RPC on a client, then ride vt+0x860. |
| `IfEntityHasBone(entity, name)` | 0x1400750a0 (SM 0x1400753e0) | | true if the model skeleton has that bone |
| `CPlayerActorPuppetEntity::GetVRHandler` | 0x140462000 (SM 0x140463e90) | | returns `player+0x25c8` (vrd) |
| `CPlayerVRData::EnableGamePlayerMovement(b)` / `EnableGlobalPlayerMovement(b)` | 0x140438330 / 0x1404382d0 (SM 0x1404384b0 / 0x140438450) | bool | vrd+0x2bc bit 0x10 / bit 0x8 |

### 1.3 `mount` and the riding state [D]
**`mount(rider, ride, seatIdent)` 0x1402ae7c0** does nothing unless three conditions hold:
- `ride.en_pnfNetFlags & PNF_ALIVE (0x10000)`;
- `rider.en_pmch` (+0x1c8) ≠ 0;
- `ride.en_pmch` ≠ 0.

When they hold:
1. It builds a **`CPSRiding`** state (0x1405da6e0: class global 0x142289cc0, size 0x90, vtable 0x14195b0e0,
   `es_henRide` +0x28, `es_idSeat` +0x38, ride placement +0x48).
2. It hands that state to the rider's state machine (`0x140ba6bd0(rider, ctx, state)`).

Failures are silent. A dead or spawner-template vehicle (no PNF_ALIVE) is the most likely reason vehicle 2 did not
board **[I]**. Filter with `v:IsAlive()`.

The riding state then does the following:
- **Enter** (state vt+0x90, 0x1405daff0; SM 0x1405dae20):
  - seat = ride vt+0x780(seatIdent) = **0x1402a2fa0**. If the seat is missing it logs `"Can't find data for seat
    '%1' in ride '%2' - %3"`. That string was never logged in the lab.
  - It moves the rider to the seat (0x1405da090, below) when the rider is not yet linked.
  - It adds the rider to `en_eaRiders` and calls rider vt+0x830 (player 0x1404864c0: creates the
    `CVehicleHandlingMotionHandler` at player+0x24b0, the riding view prompt and so on).
  - It turns the engine on (vt+0x808) when the operator is the rider.
- **Place on seat** (0x1405da090; SM 0x1405d9ec0):
  - unless the seat has `SF_RIDER_CAN_SHOOT_OWN_WEAPON`, it puts the weapon away (rider vt+0x7e0);
  - it places the rider at the seat attachment (vt+0x7b0) and attaches it (vt+0x6d0);
  - on success it writes **rider `en_henRide` (+0x75c)** and **`en_idSeat` (+0x770)**, links the mechanisms
    (0x140c343a0) and sets rider +0x2c0 = seat.
- **Operator:** `en_henOperator` (+0x428) is written inside the riding code at 0x1405db4f0 (in the function before
  0x1405db7c0). The seat's `psd_idOperatedFromSeat` is the seat itself on every SS4 vehicle (section 1.4).
  How the operator is chosen in detail was not traced **[I]**.
- `CPuppetEntity::DeleteCurrentMechanism` 0x14029f030 (SM 0x14029edf0) frees `en_pmch` and zeroes `en_pmch`,
  `en_pbo` and +0x1d8. Which riding step calls it was not traced. The crash proves `en_pbo` is NULL once the mount
  starts **[L][I]**.

### 1.4 Seats in content [M]
`CPuppetSeatData` (reflection): `psd_idSeatName` +0x8, `psd_idSeatAttachment` +0x10, `psd_idOperatedFromSeat` +0x18,
`psd_sfSeatFlags` +0x20, `psd_psvPreferredSeatView` +0x24, `psd_idRiderMechanism` +0x28, anims +0x30 / +0x38.

The rows below were read from the serialized seat record in each `.ep` (IDENT pool index → name):

| .ep | name | attachment | operated from | flags |
|---|---|---|---|---|
| SS4 `ATV_01` | **Default** | DriverSeat | Default | 0xc125 |
| SS4 `CombineHarvester` | **Default** | DriverSeat | Default | 0xc02d |
| SS4 `Tractor` | **Default** | DriverSeat | Default | 0xc105 |
| SS4 `Bike_Chopper` | **Default** | DriverSeat | Default | 0xc12d |
| SS4 `Mech` / `MechMilitary` | **Default** | Seat | – | 0x468c |
| SM `Tractor`, `TransportCart` | Default | DriverSeat | Default | 0xc105 |
| SM `Babymech` | Default | Seat | – | 0x4684 |
| SM `Snowmobile_v2` | **Ride** (?) | DriverSeat | Default | 0xc125 |

- Flag names come from the `SeatFlags` enum (0x1416d5ad8).
  - ATV 0xc125 = RIDER_CAN_SHOOT_OWN_WEAPON | PUPPETS_CANT_SEAT | PLAYER_CAN_BE_DAMAGED | FORCE_3RD_PERSON_VIEW |
    ALLOW_FIRST_PERSON_VIEW | ALLOW_THIRD_PERSON_VIEW.
  - **FORCE_3RD_PERSON_VIEW (0x100) is set on the ATV, Chopper and Tractor.** This matters for R7's "keep the
    riding view first person" item.
- The Mech row matches `MechRespawning.lua` (`MountRide(mech, "Default")`), which supports the field alignment.
- The Snowmobile row is odd: its name reads `Ride` but "operated from" reads `Default`. Use `GetSeatName(0)` rather
  than a literal.

## 2. Answer to "why did the use button not board"
- **06_Countryside's Harvester has usage disabled at level start.** `06_Countryside_ChapterControl.lua` line 4 calls
  `vehicle_Combine:DisableUsage()`. It is enabled only in the Filibert-farm chapters. So the usability check
  (0x14075adc0 → 0) refuses any use **[M][D]**.
- Other refusal reasons on that path:
  - VF_LOCKED without a satisfied key aggregator;
  - an existing rider;
  - `can_ride` (distance to `seat+0xac` and the `PlayerRideSelection` script when `pp_bRidingAlwaysAllowed` is 0);
  - the use target must be the vehicle. The VR use ray comes from the hand (0x14043dcb0, R8), so the aim matters.
- Even a successful use would have crashed in VR (section 3).

## 3. The VR crash while riding (must be fixed before any vehicle test) [D][L]
- **vrd tick 0x1404399c0** (SM 0x140439b40, short signature only) steps the two VR locomotion handlers in vrd+0x278
  (count +0x280), at vt+0x10, at 0x140439c9c. The loop runs only when all of these hold (0x140439b8d..0x140439bcd):
  - `prj_bVRWeaponAdjustMode` == 0;
  - [0x142285e54] == 0;
  - vrd+0x2ac == 0;
  - **(vrd+0x2bc & 0x18) == 0x18**.

  **Nothing in the tick checks for riding.**
  - Handler 0 step **0x14044d6c0** (vtable 0x141849c28; SM 0x14044d810, vtable 0x14182d608): at 0x14044d96e it calls
    **0x14043aae0(vrd)** (SM 0x14043ac60). That function reads `*(*(vrd+0)+0x1d0)+0xf0`, i.e. player →
    `en_pbo` → time, and compares it with the threshold global 0x142248744. **It has no null check.**
  - Handler 1 step 0x14044d530 (vtable 0x141849c78; SM 0x14044d680, vtable 0x14182d658): it calls the native turn
    handler 0x14044e880. It also calls 0x14043aae0 when `vr_bStepMovement` is set.
  - Other handler slots also read `player+0x1d0` without a check: 0x14044a960, 0x14044a9b0, 0x14044aa50, 0x14044abe0,
    0x14044e524 and 0x14044e757. Blink teleport 0x14044f4a0 (gated by `vr_bBlinkTeleport`) does too.
- **Fix: clear vrd+0x2bc bit 0x10 while the local player rides, and restore it afterwards.** The native script call
  for this is `CPlayerVRData::EnableGamePlayerMovement(false)`. Nothing native sets the bit back each tick: its
  writers are vrd init 0x14043e330, reset 0x140439120 and the script setters.
  - In Lua: `player:GetVRHandler():EnableGamePlayerMovement(false)`.
  - In the mod: set `*(uint32_t*)(vrd+0x2bc) &= ~0x10` when `handle(player+0x75c)` becomes non-null **or**
    `player+0x1d0 == 0`, and restore the previous bit on dismount. SM uses the same vrd offset (R3: only the vrd
    pointer moves, from player+0x25c8 to +0x25d0; the SM setter 0x1404384b0 writes +0x2bc).
  - This also turns off native VR turning while seated, which R7 §6(a) already recommends.
  - A narrower alternative is a null guard in 0x14043aae0, but it would leave the other unchecked readers listed
    above.
- Check also whether the mod's own vrd-tick hook (`symbols_gen` lists 0x4399c0) runs the handlers by itself.

## 4. Boarding from native code (simulation thread, lab only)
On the local-player tick that already runs `vehicles::step` / `lab_lua`:

```cpp
// Sam4 / SM
// mount        0x1402ae7c0 / 0x1402ae5e0     void (CPuppetEntity* rider, CPuppetEntity* ride, IDENT seat)
// seat by idx  vehicle vt+0x770 -> 0x1402a2f50 / 0x1402a2d10   CPuppetSeatData* (ride, int)
// use_entity   0x14049bc80 / 0x14049c270     void (player*, uint32 handle)   (full gameplay path, 1.1)
uint32_t& mv = *(uint32_t*)(vrd + 0x2bc); savedBit = mv & 0x10; mv &= ~0x10u;      // section 3, first
void* seat = vcall<void*(*)(void*,int)>(vehicle, 0x770)(vehicle, 0);
if (seat && (*(uint32_t*)(vehicle+0x7a4) & 0x10000) && *(void**)(player+0x1c8) && *(void**)(vehicle+0x1c8))
    mount(player, vehicle, *(void**)((uint8_t*)seat + 8));
// later: handle(player+0x75c) == vehicle, handle(vehicle+0x428) == player  -> vehicles.cpp engages
```

None of these is in `signatures.json` yet. Suggested additions are below. Their uniqueness was checked by `port_list`
only for the Sam4 → SM pair, not in all four exes.
- `mount_ride`: the last call of `CPuppetEntity::MountRide` (D in both builds).
- `vr_loco_in_air`: src 0x14043aae0, 24 bytes.
- `vr_loco_step0`: src 0x14044d6c0, 24 bytes.

## 5. Recommended lab procedure

**Before the run:** ask the user, and snapshot the saves (memory note "lab saves snapshot"). Each `lab_lua` run here
triggered "Requesting auto save" and wrote `QuickSave` files into the lab UserData.

**ATV handlebar test (SS4):** `+level Content/SeriousSam4/Levels/01_PB/06b_Countryside.wld`, `lab_lua_delay` ≥ 30.

**Chopper or Harvester test:** `06_Countryside` with the same chunk. Replace the `Handlebar` filter with `Steering`
for the Harvester or Tractor. The Chopper has no scheme bone (R7), so for it filter on `v:GetCharacterClass()` or
the position.

Mod-authored chunk:

```lua
local wi = worldGlobals.worldInfo
local player = wi:GetPlayer()
local vr = player:GetVRHandler()
if vr then vr:EnableGamePlayerMovement(false) end     -- avoids the en_pbo crash (section 3)
local target
for i, v in ipairs(wi:GetAllEntitiesOfClass("CVehicleEntity")) do
  local p = v:GetPlacement():GetVect()
  conInfoF("lab_lua: veh " .. i .. " alive " .. tostring(v:IsAlive()) .. " riders " .. v:GetCountOfRidersInRide()
           .. " handlebar " .. tostring(IfEntityHasBone(v, "Handlebar")) .. " seat " .. v:GetSeatName(0)
           .. " at " .. p.x .. " " .. p.y .. " " .. p.z .. "\n")
  if not target and v:IsAlive() and v:GetCountOfRidersInRide() == 0 and IfEntityHasBone(v, "Handlebar") then
    target = v
  end
end
if target then
  local qv = target:GetPlacement(); qv:SetVect(qv:GetVect() + mthVector3f(0, 0.5, 2.5))
  player:TeleportQV(qv, false)
  target:Unlock(); target:EnableUsage()                 -- only needed for the UseEntity path
  player:MountRide(target, target:GetSeatName(0))       -- or player:UseEntity(target) to test the gameplay path
  Wait(Delay(1))
  conInfoF("lab_lua: ride " .. tostring(player:GetRide() == target) .. " operator "
           .. tostring(target:GetOperator() == player) .. "\n")
end
```

- Expect `ride true operator true`.
- Then `vehicles.cpp` should see `en_henRide`/`en_henOperator`. Steering input is the left stick (R7).
- If the view is forced to third person (seat flag 0x100), check what R7's riding-view item does in VR.
- To dismount: `player:ExitRide()`. Restore the movement bit afterwards with `vr:EnableGamePlayerMovement(true)`,
  or let the mod restore it.

## 6. Unknowns
1. Which riding step calls `DeleteCurrentMechanism`, and whether `en_pbo` comes back after `ExitRide`. If it does
   not, re-enabling locomotion after dismount would crash again. Gate the restore on `player+0x1d0 != 0`.
2. `0x140aa7980(world)` in `can_ride` (gates the PlayerRideSelection script); `seat+0xac` (taken to be a use radius).
3. Whether level entity names such as `atvSpawner` or `vehicle_Combine` resolve as globals inside a `lab_lua` chunk.
   They should, since the chunk shares the content scripts' protected environment (R20), but this is not checked.
   The chunk above does not depend on it.
4. The exact ATV position in 06b. No `.wld` placement parser exists. Read it at runtime from the chunk log.
5. The SM addresses marked "short signature only" (0x140439b40, 0x14044d680, 0x14075a690) matched with 24 bytes only.
   The handler steps were confirmed by their vtable slot (+0x10 of adjacent vtables, the same layout as Sam4).

## 7. Address table

| item | Sam4 616154 | SamSM 628492 | method / conf. |
|---|---|---|---|
| MountRide glue / mount core | 0x14028dc20 / **0x1402ae7c0** | 0x140282b40 / **0x1402ae5e0** | harvest; SM core = last call in glue. D |
| EnterIntoRide glue (stub) | 0x14026f1f0 | 0x14026f740 | harvest; D |
| UseEntity glue / use_entity | 0x14045b7f0 / 0x14049bc80 | 0x14045d300 / 0x14049c270 | harvest; SM from glue call. D |
| puppet "used by" / vehicle "used by" (vt+0x448) | 0x1402b8f80 / 0x14075d3a0 | 0x1402b8dd0 / 0x14075cc30 | port_list (24 B) / SM vtable 0x141a6c9f8. D/M |
| vehicle usability (vt+0x438) / prompt text | 0x14075adc0 / 0x14075ae80 | 0x14075a690 | SM vtable slot. D |
| can_ride (vt+0x760) / choose seat (vt+0x788) | 0x1402ad020 / 0x1402a2ba0 | 0x1402ace40 / 0x1402a2960 | port_list + SM vtable. D |
| seat by index / by name (vt+0x770/0x780) | 0x1402a2f50 / 0x1402a2fa0 | 0x1402a2d10 / 0x1402a2d60 | SM vtable. D |
| CPSRiding new / enter / place on seat | 0x1405da6e0 / 0x1405daff0 / 0x1405da090 | 0x1405da510 / 0x1405dae20 / 0x1405d9ec0 | port_list. D/M |
| DeleteCurrentMechanism | 0x14029f030 | 0x14029edf0 | port_list. D/M |
| vrd tick / loco handler 0 step / 1 step | 0x1404399c0 / 0x14044d6c0 / 0x14044d530 | 0x140439b40 / 0x14044d810 / 0x14044d680 | port_list + vtable slot. D/M |
| en_pbo reader (crash site 0x14043aaf5) | 0x14043aae0 | 0x14043ac60 | port_list; SM reads +0x1d0 too. D |
| GetVRHandler / EnableGamePlayerMovement / EnableGlobalPlayerMovement | 0x140462000 / 0x140438330 / 0x1404382d0 | 0x140463e90 / 0x1404384b0 / 0x140438450 | harvest. D |
| EnableUsage / DisableUsage | 0x14028c270 / 0x14028c430 | 0x140281190 / 0x140281350 | harvest. D |
| Lock / Unlock / IsLocked | 0x140755800 / 0x140755840 / 0x140755880 | 0x1407550b0 / 0x1407550f0 / 0x140755130 | harvest. D |
| offsets: en_pbo, en_pmch, en_henRide, en_idSeat, en_henOperator, en_pnfNetFlags, en_eaRiders | +0x1d0, +0x1c8, +0x75c, +0x770, +0x428, +0x7a4, +0x2a8 | same | meta_members (SM checked). M |
| vehicle flags / key aggregator | +0xc80 / +0xd40 | +0xc88 / (not checked) | meta; SM flags from 0x14075cc30. M/D |
| vrd movement bits | vrd+0x2bc (0x8 global, 0x10 game) | same | setters. D |
| enums | PNF_ENABLED_TO_USE 4, PNF_ALIVE 0x10000, VF_LOCKED 2, SF_* 0x1416d5ad8 | | meta_enum. M |
