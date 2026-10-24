# Gate R17: Sirian Artifact of Might held in the VR hand, crushed on input (Sam4.exe 616154, SamSM.exe 628492)

Static analysis only. Sources:
- A private Ghidra copy of `lab/ghidra_r16` (Sam4.exe) and capstone (`tools/disasm.py`).
- The harvest JSONs, `meta_class.py`, `meta_members.py`, `meta_enum.py`, `port_list.py` and `sig_at.py`.
- The game's own Lua `ItemCratePickup.lua`: SS4 (`lab/lua/...`) and SM (`Content/SeriousSamSM/Scripts/`).
- The binary-meta anim sets `SirianArtefact_Arms_Animations.ans` (SS4) and `SirianArtefact_SMAnimations.ans` (SM).
  These were extracted privately and decoded with a throwaway IDNT/DTTY/OBTY reader.

Nothing was run, nothing was committed, and no game file was modified. Addresses are Sam4.exe PE VAs unless they are
marked **SM**. "state" means the `CPSGenericWeaponAnimation` puppet state object. "aq" means a model instance's
anim queue (`0x140c10420(mi, create)`, R9/R15).

Confidence tags:
- **[D]**: read from a decompile or disassembly.
- **[M]**: from reflection or content.
- **[I]**: inference that needs a runtime check.

## 0. Answers

1. **`prjStartGenericWeaponAnimation`** (S4 0x1405b05c0, SM 0x1405b0490) is a Lua binding.
   - Arguments: player, weapon params, anim name, `GWASF_*` flags. The flags are parsed from the string into the
     enum below.
   - It does not play anything itself. It pushes a new **puppet state `CPSGenericWeaponAnimation`** onto the
     player's state machine (`0x140ba6bd0`, SM 0x140ba72b0). The state is 0xd0 bytes. Its ctor is 0x1405b0c90 and
     its vtable is **0x141943710** (SM ctor 0x1405b0b60, vtable **0x141927810**).
   - The state creates a weapon entity of class **`CAnimatedDummyWeaponEntity`** from `SirianInjector.ep`
     (`CAnimatedDummyWeaponParams`, model `SirianArtefact_Arms.mdl`). It plays `"Use"` on that weapon's model
     instance with the engine anim queue. **[D][M]**
   - `"ArtifactBroken"` is a **`CGlobalScriptAnimEvent`** at frame 92 (SS4) / 91 (SM) of `Use`. The anim queue
     fires it from the weapon model. Its handler (S4 0x14019c9f0, SM 0x14019d5d0) posts a *global* script event
     named `gsae_idEvent`. That is exactly what `Wait(CustomEvent("ArtifactBroken"))` (no entity) receives. **[D][M]**
   - `"Inhale"` is a sound event (`CSamSoundAnimEvent`), not a script event. **[M]**
   - The state ends on an engine **timer equal to the anim length**, `CEntityState::Return`, scheduled only on the
     authority. When it ends, it restores weapons and signals `GenericWeaponAnimationFinished` on the player. **[D]**
2. **Recognition on the simulation thread.**
   - Hook the state's step, vtable `+0x48` = **0x1405b1740** (SM **0x1405b1610**), and its event handler, `+0x40` =
     **0x1405b1270** (SM **0x1405b1140**). Both functions belong only to this class.
   - In the hook, check these fields:
     - state `+0x18` is the local VR player;
     - `es_idAnimation` (+0x28) equals IDENT `Use`;
     - `es_bStartedPlayingAnim` (+0x6c) has become 1;
     - the right-hand weapon (player +0xf90 / SM +0xf98) has `en_pwpWeaponParams` (+0xa8). Its resource file name
       (params +0x10 → +0x18) contains `SirianInjector` (SS4 `SirianInjector.ep`, SM `SirianInjector_SM.ep`).
   - Optionally, read the crush frame at runtime from the CAnimation's event holder (§3.3). **[D][M]**
3. **Hold and release, the cleanest way.** The hold is two native levers. The anim events, VFX, Netricsa tab and
   weapon restore all stay native.
   - **(a) Anim time.** Use the engine's anim-queue speed setter `aq_set_speed` **0x140ca9450** (SM 0x140ca9b20). It
     stores `aq_fSpeedMultiplier` (+0x44 of the queue implementation) and **rebases every playing anim so its
     current time is preserved**. Set it to ≈1e-4 at the hold frame and back to the saved value on crush. A literal
     0 is not allowed because the setter divides by it.
   - **(b) The end timer.** On the authority, cancel the state's `Return` timer when the anim starts, using the
     native cancel the state's own interrupt path uses. Re-arm it on crush with `L − t_hold`.
   - Anim timing: SS4 `Use` is frames 0..210 × 0.033 s (6.93 s), with `ArtifactBroken` at **frame 92 = 3.04 s**. SM is
     0..190 × 1/30 s (6.33 s), with `ArtifactBroken` at **frame 91 = 3.03 s**.
   - `GWASF_STOP_MOVING`/`STOP_LOOKING` clear `PEAF_APPLY_OPERATOR_MOVE` (0x8) and `PEAF_APPLY_OPERATOR_LOOK` (0x20)
     in player `en_ulPuppetExecutionAbilities` +0x3a4 for the state's lifetime. Holding lengthens that.
   - The HMD view is not driven by operator look **[I]**. Locomotion and stick turning are blocked natively.
   - The dummy weapon is an ordinary `CWeaponEntity`. It is placed by the shared weapon placement 0x1402d8ad0, so its
     arms model **follows the VR controller like any weapon** **[D]**. How the canned `Use` arm motion looks at the
     hand is a runtime check.
4. **Siberian Mayhem** uses the same native state, the same flags and the same content structure, with its own script
   and assets. Differences:
   - **The skill point is granted only after `ArtifactBroken`** (in SS4 it is granted before the animation). Holding
     therefore also delays the reward.
   - A New Game+ path skips the animation entirely (no artifact in hand, nothing to hold).
   - Used-by-player bookkeeping and locked crates (`HandleLockedSkillPointPickup`) end in the same function.
   - Different anim set and frame rate (above). The skeleton is embedded in the SM `SirianArtefact_Arms.mdl`.

## 1. Native flow (both games)

### 1.1 Script (read fully)
`OnPlayerUsedSkillPointCrate(penPlayer, composite, crate, resSkillPoint, pwpWeapon)` runs on **every machine**. It is
started by the reliable RPC `OnPlayerUsedSkillPointCrate_Net`, which the crate's local `Used` handler calls.

1. The host remembers the right/left weapon params.
2. `TakeItemFromCrate`:
   - the host puts the weapons down;
   - it orients toward the crate (`EnableOperatorMove/Look(false)`, `SetLookDir`);
   - it opens the crate and waits 1.3 s;
   - it re-enables move/look and auto-animations;
   - the host runs `prjStartGenericWeaponAnimation(HandsWeapon, "TakeItem01", STOP_MOVING|STOP_LOOKING)`;
   - all machines wait for `GenericWeaponAnimationFinished`.
3. `RunAsync`:
   - `Wait(CustomEvent("ArtifactBroken"))`, a **global** event, so any player's crush on that machine wakes it;
   - spawn `SirianArtifactCrushedVFX` at the look origin, then wait 0.5 s;
   - add the PP layers `SirianArtifact`/`…Darkening` and ramp them over 3 s;
   - an inner `RunAsync`: wait 0.5 s, remove the PP layers, `SelectWeapon`/`SelectWeaponLeft(previous)`;
   - the host calls `InvokeNetricsaOnTab(1)` about 3.5 s after the crush.
4. Host: `SignalEvent(composite,"Used")`, then `prjGiveItemToPlayer(resSkillPoint)`, then
   `prjStartGenericWeaponAnimation(pwpWeapon,"Use","GWASF_STOP_MOVING|GWASF_STOP_LOOKING|GWASF_GIVE_NEW_WEAPON")`.

SM differences (from a diff of `Content/SeriousSamSM/Scripts/ItemCratePickup.lua` against SS4):
- An early return if `composite["UsedBy_"..id]` is set (`SM_HideSkillPoint`).
- If `worldGlobals.SM_bNewGamePlus` is set, there is no animation: the skill is given, the weapons are restored and a
  HUD tip is shown.
- Otherwise `prjGiveItemToPlayer`, `UsedBy_`, `SignalEvent(penPlayer,"SkillsChangedEvent")` and
  `InvokeNetricsaOnTab(1)` all run **inside** the `ArtifactBroken` handler on the host. Only `Used` and
  `prjStartGenericWeaponAnimation` run up front.
- The crate composite is `Content/SeriousSamSM/Databases/CompositeEntities/SkillPointCrate.rsc` or
  `Presets/CompositeEntities/GadgetCrateSAM.rsc`. Its `pwpWeapon` is `Content/SeriousSamSM/Databases/Weapons/SirianInjector_SM.ep`.
  The SS4 one is `GadgetCrateSAM.rsc` → `Content/SeriousSam4/Databases/Weapons/SirianInjector.ep`. **[M]**

### 1.2 Flags `GenericWeaponAnimationStateFlags` [M]
Enum run: S4 0x141941bf0, SM 0x141925cf0. Values are identical in both builds.

| flag | value | applied in |
|---|---|---|
| `GWASF_STOP_MOVING` | 0x1 | Begin: player `vt+0xd40` (stop), clear `PEAF_APPLY_OPERATOR_MOVE` 0x8 in +0x3a4. Step: if *not* set, 0x140496240 (clears player +0xfa8 bit 0x800 and +0x1d80). Player anim: two extra layer clears |
| `GWASF_STOP_LOOKING` | 0x2 | Begin: clear `PEAF_APPLY_OPERATOR_LOOK` 0x20 in +0x3a4 |
| `GWASF_INTERRUPTIBLE` | 0x4 | Step: fire bit (player +0x2338 bit 0, SM +0x2340) while started → cancel timer, end state. Not used by the artifact |
| `GWASF_LOOPING_ANIMATION` | 0x8 | Play looped. **No** Return timer is scheduled |
| `GWASF_GIVE_NEW_WEAPON` | 0x10 | Begin: if the inventory lacks `es_iWeaponIndex` (0x14041e490), give it (0x14041e870), `es_bReceivedNewWeapon` = 1 |
| `GWASF_KEEP_NEW_WEAPON` | 0x20 | End: do not remove the given weapon |
| `GWASF_RESTORE_PREVIOUS_WEAPONS` | 0x40 | End: force `weapons_restore` |
| `GWASF_AUTO_SAVE_ON_END` | 0x80 | End: autosave |
| `GWASF_CHECK_IF_BUSY` | 0x100 | Binding: abort if either hand weapon `vt+0x7b8` (busy) ≠ 0 |

`en_ulPuppetExecutionAbilities` (+0x3a4 in both builds) is the `PuppetExecutionAbilities` enum (S4 0x1416cf8d8):
1 `MACRO_ACTIONS`, 2 `LERP_TO_DESIRED`, 4 `ENABLE_ROTATION`, 8 `APPLY_OPERATOR_MOVE`, 0x10 `EVALUATE_MODEL_MOVER`,
0x20 `APPLY_OPERATOR_LOOK`. These are the same bits the crate script toggles with `EnableOperatorMove/Look`. **[M]**

### 1.3 Binding 0x1405b05c0 (SM 0x1405b0490) [D]
1. Arguments 0..3: player, params, name, flags.
2. Pick the model configuration:
   - default: `wp_pmcModel` (params +0xc8);
   - if `wp_pwsAltWeaponSkin` (+0x140) is set and player `vt+0x11f0(params)` ≠ 0: the skin's +0x60.
3. `0x1401faae0` resolves the anim IDENT in that model. If it is invalid, it returns failure.
4. `0x1405b1cc0` builds the optional body anim `"%1_%2"` (params name, anim) on the player model and checks it exists.
   The result goes to `es_idPlayerAnimation`.
5. Weapon index = `0x1409667a0(0x14024c710(player), params)`.
6. Allocate 0xd0 bytes and run the ctor 0x1405b0c90. The arguments are the anim id, the index, the flags and the
   player anim.
7. Push the state with `0x140ba6bd0(player, now, ev)`.

**The script runs it on the host only.**

### 1.4 State members (`CPSGenericWeaponAnimation`, identical offsets in S4/SM) [M]
Members marked "net" (0x4000) are replicated. A tick field follows each value.

| member | offset |
|---|---|
| state → owner puppet | +0x18 [D] |
| `es_idAnimation` (net) | +0x28 |
| `es_iWeaponIndex` (net) | +0x38 |
| `es_bReceivedNewWeapon` | +0x44 |
| `es_ulWeaponAnimationFlags` (net) | +0x48 |
| `es_ulOriginalPuppetExecutionAbilities` (net) | +0x54 |
| `es_bStartPlayingAnimNet` (net, 0x5000) | +0x60 |
| `es_bStartedPlayingAnim` (local) | +0x6c |
| `es_aidVisibleAttachments` / `es_aidHiddenAttachments` | +0x70 / +0x80 |
| `es_pgipDesiredGadget` / `PreviousGadget` (gadget variant, `prjStartGenericGadgetAnimation` 0x1405b08f0) | +0x90 / +0xa0 |
| `es_idPlayerAnimation` (net) | +0xb0 |
| `es_bOriginalAutoAnimations` (net) | +0xc0 |

### 1.5 Event handler `vt+0x40` 0x1405b1270 (SM 0x1405b1140) [D]
The event type is at `ev+8`.

**`EBegin`** (type global 0x14239ff58):
- Authority only:
  - save +0x3a4 → +0x54, and auto-animations (player +0x9f8) → +0xc0;
  - GIVE_NEW_WEAPON (above);
  - `0x140496270`: remember the current weapons into player +0x22ac / +0x22b0 (read back by `weapons_restore`);
  - `0x140484c20`;
  - desired right/left = −1 (+0x1af8 / +0x1b04);
- Everywhere:
  - the STOP_MOVING and STOP_LOOKING bits (§1.2);
  - `0x1404729a0(player, 0)`: put the current weapons away, authority only;
  - continue.

**`EEnd`** (0x14239ff60):
- `0x140474dc0`, restore +0x3a4 from +0x54, stop the body anim and restore auto-animations.
- Re-show the hidden and re-hide the visible attachments on the right weapon (0x1402de720 / 0x1402de8c0), then weapon
  `vt+0x6f0(1)`.
- Decide `remove = es_bReceivedNewWeapon && !KEEP`.
- If there is no weapon, RESTORE_PREVIOUS or `remove`: run **`weapons_restore` 0x140490bc0** (SM 0x1404912b0).
  Otherwise weapon `vt+0x6f8` and 0x14047ed10.
- Clear weapon +0x168 bit 0x200 (`DisablePutDownAnim`).
- If `remove`: `0x14041ed10` (inventory bit off).
- AUTO_SAVE: autosave.
- Finally: if a script waits for it, signal **`GenericWeaponAnimationFinished`** on the player entity. The script
  system call is `0x140b75380` (has waiter) / `0x140b75530` (signal). The string is S4 0x1419437a8, SM 0x1419278a8.

**Type 0x1422d9218** (an anim function-call event forwarded by the weapon anim-event handler 0x1402de1b0, SM
0x1402ddce0) with IDENT `RememberAttachments` (0x142289768) or `DisablePutDownAnim` (0x142289770): it records
attachment visibility, or sets weapon +0x168 |= 0x200. Neither appears in the artifact anim.

### 1.6 Step `vt+0x48` 0x1405b1740 (SM 0x1405b1610) [D]
Runs each simulation tick while the state is active.

**1. Interruptible case.** If INTERRUPTIBLE is set, the fire bit is set and the anim has started:
- `make_handle(&h, state, state->vt[0](state))` (0x1410de9b0);
- `timer_cancel(*(*(state+0x18)+0x30), &h)` (0x140c32490; `state_world` 0x140d05bb0 is that load);
- `state_end` 0x140d05db0.

**2. Before the start** (`es_bStartPlayingAnimNet` = 0, authority only):
- Wait until player `vt+0x1150` and `vt+0x1148` (current left/right weapon index) are both −1.
- Then **`0x140471f50(player, es_iWeaponIndex, 1)` creates the dummy weapon entity**. Its third argument is stored
  in the creation record (+0x84), which is probably the right hand **[I]**.
- Set player +0xf90 to its handle, and desired +0x1af8 / +0x1b24 to the index.
- Weapon `vt+0x938(1)`; register its tools (0x1402a1070).
- Set `es_bStartPlayingAnimNet` = 1. This is replicated.

**3. Start**, on every machine once `es_bStartPlayingAnimNet` is 1 and `player vt+0x1148 == es_iWeaponIndex`:
- `es_bStartedPlayingAnim` = 1;
- `weapon = handle_entity(player+0xf90)`, `aq = 0x140c10420(weapon+0x90, 1)`;
- if the model has the anim: weapon `vt+0x6f0(0x10)`;
- `0x140baef50(aq, 0)` (new clear list);
- **`aq_play` 0x140baf450(aq, id, looping, 1.0, 1.0, 0)**;
- weapon `vt+0x860`.
- If not looping and authority: **`len = aq_anim_length 0x140bae480(aq, id, 0)`** (`(iLast−iFirst)·fSecPerFrame`,
  0x140ca9c00). Then **`state_schedule 0x1400827c0(state, len, CEntityState::Return 0x140d053d0,
  "CEntityState::Return")`**.
  - SM: `aq_anim_length` 0x140baeb90, `state_schedule` 0x140082aa0, `Return` 0x140d05a30.
  - String: S4 0x141941b28, SM 0x141925c28.
- If `es_idPlayerAnimation` is valid: play it on the player model (+0x1e0) with layer clears.

**The end time is fixed at the start and does not depend on the anim queue's speed.**

### 1.7 Anim event → CustomEvent [D][M]
- Event classes found in the anim set: `CSamSoundAnimEvent`, `CShowPlayerWeaponAttachmentAnimEvent`,
  `CHidePlayerWeaponAttachmentAnimEvent` and `CGlobalScriptAnimEvent`.
- Base `CAnimEvent`: `ae_fFrame` +0x18 (frame number, float), `ae_ulFlags` +0x1c, `ae_bOnlyInFirstLoop`.
- `CGlobalScriptAnimEvent::gsae_idEvent` +0x48.
- Its vtable is S4 0x141649fb0 (SM 0x14162ca60). Slot +0x18 is the fire handler, **0x14019c9f0** (SM 0x14019d5d0).
- The fire handler takes the entity at `ctx+0x10` (the weapon) and gets the script system of that entity's world
  (0x140b79820).
- If `0x140b75380` reports a waiter for `gsae_idEvent`, it allocates a 0x20-byte `CGlobalScriptEvent`
  (vtable 0x141649f60) with +0x10 = the entity's handle and +0x18 = the IDENT. It then posts it (0x140636c60).
- **The event comes straight from the anim queue's event processing, not through the weapon or the state.** So it
  fires on every machine whose copy of the weapon model plays `Use` (§1.8).

### 1.8 Host and client [D for authority gating, I for replication details]
- Authority (SP, host) only: the binding call (the script runs it on the host), Begin's inventory and weapon bookkeeping,
  creating the dummy weapon, and the **`Return` timer**.
  - `is_authority` 0x140ba6dd0 (SM 0x140ba74b0): world +0x1c0 → +0x230 net object, `vt+0x68 || vt+0x60`.
- **A client** receives the replicated state (`es_*` net members).
  - Once `es_bStartPlayingAnimNet` arrives and its current right weapon index matches, it plays `Use` on its own copy
    of the weapon model. Its local `ArtifactBroken` then drives its own VFX and PP (the Lua runs everywhere).
  - Its state ends when the host's state returns.
- The host fires `ArtifactBroken` from *its* copy of the remote player's weapon. That is what triggers the host-side
  `InvokeNetricsaOnTab(1)` (and in SM the skill grant) for that client.
- Native quirk: `CustomEvent("ArtifactBroken")` is global, so two players crushing at the same time cross-trigger.

## 2. Content facts [M]
| | SS4 | SM |
|---|---|---|
| weapon params | `Content/SeriousSam4/Databases/Weapons/SirianInjector.ep` (`CAnimatedDummyWeaponParams`) | `Content/SeriousSamSM/Databases/Weapons/SirianInjector_SM.ep` |
| model | `Models/SirianArtefact/SirianArtefact_Arms.mdl` (skeleton `Weapons/Arms/Resource/Arms.skl`) | `SeriousSamSM/Models/SirianArtefact/SirianArtefact_Arms.mdl` (embedded skeleton, `Root` bone) |
| anim set | `SirianArtefact_Arms_Animations.ans` | `SeriousSamSM/Models/SirianArtefact/Animations/SirianArtefact_SMAnimations.ans` |
| `Use` | `an_iFirstFrame` 0, `an_iLastFrame` 210, `an_fSecPerFrame` 0.033 → **6.93 s** | 0, 190, 0.03333 → **6.33 s** |
| events (frame) | Show attachment `SirianArtefact` 0; sound `Crushing` 0; sound `Trying` 50; **Hide `SirianArtefact` 91**; **`ArtifactBroken` 92**; sound `Inhale` 130 | Show 0; `Crushing` 2; `Trying` 50; Hide 91; **`ArtifactBroken` 91**; `Inhale` 130 |
| sounds (SS4 sound scheme) | `Crushing` → `Player_CrushingSirianArtefact.wav` 4.54 s (grunt); `Trying` → `CrushingSirianArtifact.wav` 1.57 s (crack); `Inhale` → `Inhale02.wav` 1.01 s | same terms |

- Model children: `SirianArtefact` (on `R_Weapon`, `SirianArtefact.mdl`), `ArtefactBrokenVFX` (on `R_Weapon`) and `Arms`
  (`Weapons/Arms/Arms.mdl`).
- Params: `wp_pmcVRModel` = NULL, `wp_pwrWeaponRenderingParamsVR` = NULL,
  `wp_vVRHandsOffsetFromHead` = (0.2, −0.15, 0.15), `wp_bUsesSingleArmAnimations` = TRUE,
  `wp_bTwoHanded` = FALSE, `wp_bManuallySelectable` = FALSE.

Reflection offsets used here are the same in both builds:
- `ae_fFrame` +0x18;
- `an_iFirstFrame` +0x8, `an_iLastFrame` +0xc, `an_fSecPerFrame` +0x10, `an_paehHolder` +0x60;
- anim queue implementation (`aq+8`): `aq_aplLists` +0x8, `aq_fSpeedMultiplier` +0x44, `aq_fFadeSpeedMultiplier`
  +0x48;
- `mi_paqAnimQueue` +0x10, `mi_pmcConfiguration` +0x18;
- `en_pwpWeaponParams` +0xa8, `en_bRightHandWeapon` +0x118;
- `rs_prf` +0x10, `rf_strAssetFN` +0x18;
- `gsae_idEvent` +0x48.

The event times were decoded from the binary meta. Member order follows the DTTY STMB lists, and the CAnimation
values match the `0x140ca9c00` field use. **[M], high confidence.** Frames are in anim frames, not seconds.

## 3. Holding the animation

### 3.1 The anim queue clock [D]
- A played anim entry is 0x60 bytes. It sits in the list at `aq+8 → +0x8`; each list is 0x28 bytes with its anims at
  +0x18 and the count at +0x20. Fields:
  - +0x08 start time (64-bit 32.32 fixed, against the global clock `[0x1422d4ac8]`);
  - +0x10 strength;
  - +0x14 time offset;
  - +0x18 speed;
  - +0x20 anim IDENT;
  - +0x38 looping;
  - +0x3c group.
  The play implementation is 0x140ca8a50.
- **`aq_set_speed` 0x140ca9450** (SM 0x140ca9b20):
  - It is `void(aq, float s)`.
  - `old = imp+0x44; imp+0x44 = s; r = old/s`. Then for every anim entry: `start = now − (((now−start)·2⁻³² +
    off)·r − off)·2³²`.
  - The anim time `((now−start)+off)·mult` is therefore preserved. It is a native "change speed without a jump".
- **`aq_get_speed` 0x140ca8590** (SM 0x140ca8c60) returns `imp+0x44`.
- Both are used by the puppet speed scaler 0x1402b7110 (calls #6 and #7), which also rescales the state timers
  with 0x140d05700.
- Pass a small positive value; never 0. At 1e-4, `Use` advances 0.1 ms per real second. The 64-bit rebase
  (`cvttss2si r64` fallback) and float precision stay well within range.
- The anim events are assumed to be evaluated in anim time, so they do not fire while the multiplier is tiny **[I]**.
  This is the one behaviour to confirm in the lab: log `0x14019c9f0` hits during a hold.

### 3.2 Hold sequence (simulation thread, in the step hook)
1. **Detect** (§0.2). Detection happens on the tick where `es_bStartedPlayingAnim` goes 0→1. The real step has just
   called `aq_play` and, on the authority, `state_schedule`. Then:
   - `aq = mi_anim_queue(*(weapon+0x90), 0)`;
   - `L = aq_anim_length(aq, *(IDENT*)(state+0x28), 0)`;
   - `t0 = now`;
   - `s0 = aq_get_speed(aq)`, normally 1.0.
2. **Authority:** cancel the native end timer at once.
   - `world = *(*(state+0x18)+0x30)`;
   - `make_handle(&h16, state, state->vt[0](state))`;
   - `timer_cancel(world, &h16)`.
   This is the same sequence as the INTERRUPTIBLE path, §1.6. Only this state's scheduled calls are keyed by that
   handle.
3. When the anim time reaches the hold frame `H·fSecPerFrame` (track `now − t0`, or read the entry), call
   `aq_set_speed(aq, 1e-4f)`. The artifact is now in the hand, posed and still.
4. **Release**, on the crush input:
   - `aq_set_speed(aq, s0)`;
   - authority only: `state_schedule(state, L − t_held_anim, Return, "CEntityState::Return")`, using the native
     string VA. `t_held_anim` is the anim time at release.
5. From here, everything is native:
   - `Hide SirianArtefact` (91), `ArtifactBroken` (92/91), the script VFX, PP, `InvokeNetricsaOnTab(1)` (host), the
     SM skill grant;
   - `Inhale`;
   - the state end at the original relative time (restore, inventory removal, `GenericWeaponAnimationFinished`);
   - the script's `SelectWeapon(previous)`.
6. **Safety:**
   - Clear tracking on `EEnd` (handler hook) or if the state, weapon or player is deleted.
   - Auto-release after a timeout (for example 120 s), and when the player dies or VR is turned off.
   - Never leave a queue at 1e-4 after the weapon handle changes.

### 3.3 Choosing H
- SS4/SM `Trying` (the crack sound, 1.57 s) starts at frame 50. The hand hides the artefact at 91.
  1. **H ≈ 45–48** (default; about 1.5 s). The artefact is up in the hand before the crack sound. Release → the native
     crack plus crush, `ArtifactBroken` about 1.45 s later.
  2. **H ≈ 88** (2.9 s). The crush is instant on release, but the crack sound has already played during the run-up.
- The grunt sound `Crushing` starts at frame 0/2 and lasts 4.54 s. It keeps playing during a hold because sounds are
  not paused by the queue. This is cosmetic and is accepted, or is a later mod option.
- The exact pose at H is not known statically (bone tracks not decoded). Expose H as a cvar per game.
- Runtime-robust option: read `ArtifactBroken`'s frame at start.
  - Walk the CAnimation's `an_paehHolder` (+0x60) → `aeh_caeEvents`. The `CDynamicContainer` count/pointer layout is
    **unverified**.
  - Find the event whose vtable is the `CGlobalScriptAnimEvent` one and whose `gsae_idEvent` (+0x48) is IDENT
    `ArtifactBroken`.
  - Read its `ae_fFrame` and keep H below it. The CAnimation can be found the way `0x140bae480` does it
    (`0x140e14540(&{aq[2], aq->vt[0x18]()}, id)`).

### 3.4 Alternatives rejected
- **Holding only the state** (cancelling the timer): the anim keeps running, so the crush happens at 3 s anyway.
- **Delaying the event** (hooking `0x14019c9f0`): the hand closes and the artefact hides at frame 91 before the
  delayed event. Out of order.
- **Speed 0 through the setter**: division by zero.
- **Rebasing the entry start time each tick from mod code**: possible, but it re-implements what `aq_set_speed` does
  natively.
- **Gating `es_bStartPlayingAnimNet`**: the artefact would not be in the hand during the hold.
- **Looping flag**: needs a different script call.

### 3.5 VR player during the state
- **Look/move:** native for the whole hold (§1.2). The HMD orientation comes from the device pose in the view path
  (R2/R3), and is assumed not to be gated by `PEAF_APPLY_OPERATOR_LOOK` **[I]**. The crate script already toggles the
  same bit before this, so whatever VR does during crate opening also applies here.
  - If smooth or snap turning should be allowed while holding, set bit 0x20 back for the hold only. The state's End
    restores +0x3a4 from +0x54 anyway.
- **Hand placement:** `CAnimatedDummyWeaponEntity` is built on the `CWeaponEntity` ctor 0x1402d2e50 (its ctor
  0x140764610 calls it). Its static table holds the shared functions, so the existing mod hooks by address already
  cover it:
  - placement 0x1402d8ad0 (= `weapon_placement`);
  - step/grip 0x1402de9a0 (= `weapon_step`);
  - model getter 0x1402d7f90 (picks `wp_pmcVRModel` if set; NULL here → `wp_pmcModel`);
  - arms-skin 0x1402e2910;
  - anim-event handler 0x1402de1b0.
  The model therefore sits at the controller chosen by `en_bRightHandWeapon` +0x118 like any weapon (R12 §10, R15).
  The canned `Use` arm/`R_Weapon` motion relative to the model root, and how `arms.cpp` (park/free) treats it, are
  runtime checks.
- **The body animation** `es_idPlayerAnimation` (if such an anim exists on the player model) is not frozen by the
  weapon-queue hold. Others may see it finish early. Unknown whether it exists for this weapon.

## 4. Ports
Methods: `port_list.py` (shortest unique wildcarded signature), `sig_at.py --len 96` (chk), the harvest JSONs, vtable
slots and `manual.json`.

| name | Sam4 | SM | Sam4_U | SM_U | method / chk |
|---|---|---|---|---|---|
| prjStartGenericWeaponAnimation (binding) | 0x1405b05c0 | 0x1405b0490 | – | – | harvest; chk 80/80 |
| gwa ctor | 0x1405b0c90 | 0x1405b0b60 | – | – | sig 32 |
| gwa vtable | 0x141943710 | 0x141927810 | – | – | ctor `lea` |
| **gwa_handle (vt+0x40)** | 0x1405b1270 | 0x1405b1140 | 0x1405b39a0 | 0x1405b3860 | sig 24; chk 76/76; SM vt slot confirmed |
| **gwa_step (vt+0x48)** | 0x1405b1740 | 0x1405b1610 | 0x1405b3e70 | 0x1405b3d30 | sig 24; chk 83/84 (player offsets +0x2338→+0x2340, +0xf90→+0xf98, +0x1af8→+0x1b00, +0x1b24→+0x1b2c); SM vt slot confirmed |
| gwa player-anim helper | 0x1405b1cc0 | 0x1405b1b90 | – | – | sig 24 |
| **aq_set_speed** | 0x140ca9450 | 0x140ca9b20 | 0x140cabfe0 | 0x140cac6a0 | sig 24; chk 76/76 |
| **aq_get_speed** | 0x140ca8590 | 0x140ca8c60 | 0x140cab120 | 0x140cab7e0 | sig 24 (runs into the next function); bytes checked in SM |
| aq_play | 0x140baf450 | 0x140bafb20 | – | – | sig 24; seen in SM step |
| **aq_anim_length** | 0x140bae480 | 0x140baeb90 | 0x140bb0e30 | 0x140bb1530 | sig 24; chk 88/88 |
| mi_anim_queue | 0x140c10420 | 0x140c10ba0 | – | – | existing (R15) |
| **state_schedule** | 0x1400827c0 | 0x140082aa0 | 0x140085110 | 0x1400853e0 | sig 96; SM call in step |
| **CEntityState::Return** | 0x140d053d0 | 0x140d05a30 | 0x140d07f60 | 0x140d085b0 | sig 48; SM `lea` in step |
| "CEntityState::Return" string | 0x141941b28 | 0x141925c28 | – | – | the step's `lea r9` |
| **timer_cancel** | 0x140c32490 | 0x140c32a30 | 0x140c35070 | 0x140c35600 | sig 24; chk 88/88; SM call in step |
| state_world (inline `*(*(s+0x18)+0x30)`) | 0x140d05bb0 | 0x140d06520 | 0x140d08740 | 0x140d090a0 | sig 24 |
| **make_handle** | 0x1410de9b0 | 0x1410df250 | 0x1410e1540 | 0x1410e1dd0 | sig 32; SM call in step |
| is_authority | 0x140ba6dd0 | 0x140ba74b0 | – | – | sig 96 |
| push_event (state machine) | 0x140ba6bd0 | 0x140ba72b0 | – | – | sig 24 |
| state_end | 0x140d05db0 | 0x140d06720 | – | – | sig 24 |
| gsae fire (`CGlobalScriptAnimEvent` vt+0x18) | 0x14019c9f0 | 0x14019d5d0 | 0x14019f3a0 | 0x14019ff70 | sig 24; chk 72/72; SM vt 0x14162ca60 |
| weapon anim-event handler | 0x1402de1b0 | 0x1402ddce0 | – | – | R15 |
| weapons_restore | 0x140490bc0 | 0x1404912b0 | 0x1404932f0 | 0x1404939d0 | manual.json |
| handle_entity | 0x141112c50 | 0x141113500 | – | – | manual.json |
| GWASF enum | 0x141941bf0 | 0x141925cf0 | – | – | meta_enum |

`[longer sig: no match]` was reported for `aq_anim_length` (all ports), `gwa_step` and `timer_cancel` (SM_U). The
24-byte signature is unique, but the function bodies diverge later. The SM ports were confirmed by direct call sites
in the SM step. SM_U should get the same `sig_at` check before the gate.

## 5. Unknowns and runtime checks
1. Whether anim events are suppressed while `aq_fSpeedMultiplier` ≈ 1e-4, i.e. whether events are evaluated in anim
   time. Log the `0x14019c9f0` hits and the hide at 91. This is the main check.
2. The `Use` pose at the hold frame, and how the arms look at the VR hand (`arms.cpp` handler interplay).
3. Whether the VR HMD view or snap turn is affected by `PEAF_APPLY_OPERATOR_LOOK` / `MOVE` being cleared.
4. Whether the third argument of 0x140471f50 (=1) is "right hand". Left-handed mode and the mod's hand swap.
5. Saving during a hold:
   - `aq_fSpeedMultiplier` is a reflected member and probably serialised;
   - the cancelled timer is not in `CPSGenericWeaponAnimation_SaveData` (no timer field; load-time re-arm unknown).
   Suggest refusing or deferring saves while holding, or releasing before a save.
6. MP: the host animates and fires `ArtifactBroken` for a remote player's weapon (implied by the script design, not
   proven). A VR client's hold must be mirrored on the host (§6.4).
7. The `CAnimEventHolder::aeh_caeEvents` container layout, for the runtime frame lookup.
8. Whether `es_idPlayerAnimation` exists for this weapon on the player model.

## 6. Recommended implementation

### 6.1 Signatures (`docs/re/builds/signatures.json` format)
```json
"gwa_step":        {"src": "0x1405b1740", "len": 24, "_note": "CPSGenericWeaponAnimation vt+0x48 OnStep (R17 s1.6); SM chk differs only in player offsets"},
"gwa_handle":      {"src": "0x1405b1270", "len": 24, "_note": "CPSGenericWeaponAnimation vt+0x40 HandleEvent (EBegin/EEnd) (R17 s1.5)"},
"gwa_ctor":        {"src": "0x1405b0c90", "len": 32, "_note": "stores the CPSGenericWeaponAnimation vtable (lea at +0x32)"},
"aq_get_speed":    {"src": "0x140ca8590", "len": 24, "_note": "float(aq): aq_fSpeedMultiplier imp+0x44; 3-instr fn, sig spills into the next fn but is unique in all four builds"},
"aq_set_speed":    {"src": "0x140ca9450", "len": 24, "_note": "void(aq, float): sets aq_fSpeedMultiplier, rebases entries (time preserved); never 0"},
"aq_anim_length":  {"src": "0x140bae480", "len": 24, "_note": "float(aq, IDENT, int incl): (iLast-iFirst)*fSecPerFrame"},
"state_schedule":  {"src": "0x1400827c0", "len": 96, "_note": "void(state, float delay, cb, const char* name): world timer keyed by state handle"},
"state_return":    {"src": "0x140d053d0", "len": 48, "_note": "CEntityState::Return callback"},
"timer_cancel":    {"src": "0x140c32490", "len": 24, "_note": "void(world, const handle16*): cancel the state's scheduled calls"},
"make_handle":     {"src": "0x1410de9b0", "len": 32, "_note": "void(handle16* out, obj, type=obj->vt[0](obj))"},
"gsae_fire":       {"src": "0x14019c9f0", "len": 24, "_note": "CGlobalScriptAnimEvent fire (vt+0x18); observe-only hook for logging ArtifactBroken"}
```
- `call_of` is not usable for the speed pair. Their only direct callers (0x1402b7110, 0x1402b89ee, puppet speed
  scalers) are not harvested, and 0x1402b7110 has no unique SM signature (`port_list`: not found). Use `src`.
- `aq_get_speed`'s 24-byte signature extends past its `ret`: confirm the Unrestricted hits with `sig_at` (SM was checked by disassembly).
- The `CEntityState::Return` string VA does not need a signature: read it from the `lea r9` just before the
  `state_schedule` call in `gwa_step`.
- Members go to `members.json`. All are the same in both builds except the player weapon/fire offsets, which R15
  already has:

| member | offset |
|---|---|
| state owner | +0x18 |
| `es_idAnimation` | +0x28 |
| `es_iWeaponIndex` | +0x38 |
| `es_ulWeaponAnimationFlags` | +0x48 |
| `es_ulOriginalPuppetExecutionAbilities` | +0x54 |
| `es_bStartPlayingAnimNet` | +0x60 |
| `es_bStartedPlayingAnim` | +0x6c |
| `aq_fSpeedMultiplier` (in `aq+8`) | +0x44 |
| `en_pwpWeaponParams` | +0xa8 |
| `rs_prf` | +0x10 |
| `rf_strAssetFN` | +0x18 |
| `en_ulPuppetExecutionAbilities` | +0x3a4 |

### 6.2 Module `src/se4/artifact.cpp` (shared; per-game data only H and the model/params name check)
- MinHook `gwa_step` and `gwa_handle`, like `gadgets.cpp`. Both run on the simulation thread.
- `hookedStep(state)`:
  1. Call the real step.
  2. If not tracked: when `player == local VR player`, `+0x6c == 1`, `+0x28 == IDENT("Use")` and the right weapon's
     params file name contains `SirianInjector`, start tracking. Run §3.2 steps 1–2: save `aq`, `L`, `t0`, `s0`,
     `auth = is_authority(player)`; if `auth`, cancel the timer.
  3. While tracked: freeze at H (§3.2 step 3). When the input fires, release (§3.2 step 4). Check the timeout.
- `hookedHandle(state, ev)`: on `EEnd` for a tracked state, if still frozen, restore `s0` first and clear the
  tracking. Then call the real handler.
- **Crush input:** the rising edge, on the hand holding the artifact, of either:
  - trigger value > 0.85;
  - grip/squeeze value > 0.9, or Index "squeeze force" > 0.5 (OpenXR `/input/squeeze/force`), or a grip second stage
    where the profile has one.
  Use the action set the mod already reads in `vr_device`. Optionally require the controller to be in front of the
  HMD.
- **Haptics while held:** a weak pulse on that controller. On release, a strong pulse.
- Never touch gameplay state other than the queue speed and the state's own end timer. All the rewards stay script
  and native.

### 6.3 SM specifics
- The same code path. SM `ArtifactBroken` is at frame 91 and `Use` is 6.33 s.
- The skill grant waits for the crush, so a forgotten hold would withhold the reward. The timeout auto-release is
  therefore mandatory.
- NG+: no state is pushed and no artifact appears, so there is nothing to do.

### 6.4 Multiplayer
- **The VR player is the host, or SP:** everything above is local.
- **The VR player is a client:** the host owns the timer and fires the authoritative `ArtifactBroken` (Netricsa tab;
  SM skill) from its copy. The client sends `artifact_hold{playerId}` / `artifact_release{playerId}` on the mod MP
  channel (R13).
  - The host's mod applies §3.2 to that player's state (it is authoritative, so it cancels and re-arms the timer and
    freezes its copy of the weapon queue).
  - The client freezes its own local copy so its VFX and PP match.
  - If the host lacks the mod, the client should not hold. The fallback is native timing.
- **Mixed desktop players:** no change.

### 6.5 Verification (lab Monado, both games, simulated input)
1. Use a skill crate. Log the state pointer, `L`, the detected H, and the `aq_set_speed` calls.
2. Confirm that during the hold there are no `gsae_fire` hits, no `Hide` and no `GenericWeaponAnimationFinished`, and
   that the artefact stays in the hand.
3. Trigger the crush. Confirm, in order: `ArtifactBroken` (gsae hit) → VFX/PP → Netricsa tab 1 (SS4: the skill point
   is already granted; SM: it is granted now, and `SkillsChangedEvent` fires) → `GenericWeaponAnimationFinished` →
   the previous weapons are back.
4. Timeout path, death during the hold, save attempt during the hold.
5. Coop with the VR player as host and as client.
