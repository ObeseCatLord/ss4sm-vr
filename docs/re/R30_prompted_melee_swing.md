# R30: depth partition (shrub ghosting), prompted melee by swing in VR (Sam4.exe 616154, SamSM.exe 628492)

Sources: Ghidra decompiles from a private scratch copy of `lab/ghidra_r19` (`lab/r29/m*.c`), capstone disassembly,
lab runs on the isolated lab copies (lab-only options `upscale_debug_view`, `upscale_pass_trace`,
`melee_prompt_debug`).

## 1. The scene depth is partitioned (ghosting while moving)

- **Symptom.** Under DLSS, shrubs left trailing copies ("clones following the motion") when the player moved.
- **Cause.** The engine draws with viewport depth ranges:
  - the world into **[0.1, 1]**;
  - far content into **[0, 0.1]**;
  - reversed Z, infinite far, n = 0.1. The API matrix at `gfx+0xdf0` has [10] 0, [11] 0.1, [14] −1.
  - `vkCmdSetViewport` trace: 0.000000..0.100000 and 0.100000..1.000000.
  - The mod decoded depth as if it spanned [0, 1]. A depth of 0.104 then read as ~1 m everywhere (the
    `upscale_debug_view` readback: 0.90–0.97 m across a row that is really 3–30 m away).
  - So camera motion vectors had the wrong parallax for every pixel while translating. Rotation-only motion is
    depth-independent, which is why turning looked fine.
- **Fix.**
  - The layer hooks `vkCmdSetViewport` while an upscaler may run, and records the split (minDepth of a
    `[split, 1]` range, `upint::onViewport`).
  - `motion.comp` maps a stored depth back to the projection's [0, 1] before `n/d`: `d ≥ split`: `(d − split)/(1 − split)`;
    else `d/split`.
  - In flat play, pixels nearer than 0.25 m (the first-person weapon, drawn with its own projection) get zero
    motion, since they are attached to the camera. In VR the hands are tracked, so this is off.
- **Lab after the fix.**
  - Distances read 5 m (ground), 4–15 m (bushes, house), 26–54 m (background), 35–114 m (trees); the sky reads
    infinite.
  - While strafing (`flat_stability.sh` with `MOVE=1`), the bush that smeared into overlapping copies with the old
    build has crisp leaves.

## 2. The melee prompt and its eligibility

- **Prompt.** The HUD message "%1 Melee" with `{plcmdUse}` (`melee_prompt_site`, `0x1400b29d4`, SM `0x1400b2bb4`)
  is shown while the player's **`en_pnfNetFlags` +0x7a4 has `PNF_PLAYER_MELEE_AVAILABLE` (0x200000)**.
- **The flag** is set each step on the authority by `0x1404a3670` from **`melee_find` 0x14041ff20** (SM
  `0x14041fff0`) `(player, 1, &target, &anim, &index)`, the same test the Use/Melee button runs. It is network
  replicated.
- **`melee_find` requires**, in order:
  - no CPlayerVRData: `cmp [r12+vrd], r14; jne out`, vrd +0x25c8 (SM +0x25d0). **The engine turned melee kills off
    for VR players**, which is why the prompt never appeared in VR;
  - a right-hand weapon (+0xf90 / SM +0xf98) whose `vt+0x780` allows it;
  - candidates from the world info's character list (`worldInfo+0xe0`) that are hostile (`vt+0x620` → alignment
    table `0x14008c900`). A puppet whose AI semantic (+0x98 `en_paisAISemantic`) is `CAISDecorative` is never
    hostile;
  - within `pmlSetMaxRange` (10 m), upright, inside the view cone (player FOV);
  - line of sight (`0x1401a4f20`);
  - then the player state machine's melee event in test mode (`0x1405d0c00`). This requires a physics body, and a
    stun for enemies with `php_bRequiresStunForMelee`, and runs `PlayerMeleeSelection.lua`, which lists the enemy
    classes that can be melee-killed per weapon.
- **The canned kill** is `melee_start` 0x140473b40 (SM `0x140474140`): a synchronized animation that places and locks
  the player.

## 3. Prompted melee by swing (`src/se4/melee.cpp`)

- **Finder.** The `jne` of the VR check is replaced by a 6-byte NOP. The finder, and with it the flag and the
  prompt, then answers for VR players exactly as for desktop players: same enemies, same stun rules, same script.
  This runs on every peer with the mod, so a server decides for its remote VR players too.
- **Prompt.** In VR, the prompt's `{plcmdUse}` argument (`lea r9` at site+0xf) points at an empty string: it reads
  "Melee" with no button.
- **Button.** In VR, `melee_start` called from a use action (the finder's call within 0x60 bytes before the call) is
  skipped for players with CPlayerVRData. `DoUseMeleeAttack` (scripts) and desktop players keep it.
- **Swing.**
  - Every CPlayerVRData step, while the flag is set, the finder's target is taken.
  - A hand moving faster than `melee_prompt_speed` (default 1.5 m/s, relative to the head, so walking does not count)
    is swept with the segment cast, along the hand's path and along the head-to-hand line.
  - If either cast hits **that** target, it takes `DMT_PLAYERMELEE` damage of 2 × its remaining health + 10
    (`en_slHealth` +0x574, a CSyncedSLONG with the value at +0).
  - It then dies its own death: no animation for the player, no placement, no lock.
  - `HealthAmmoRestore.lua` gives the melee health restore for that death type. Kill credit and gibs stay native.
- **Options:** `melee_prompt_swing` (default 1), `melee_prompt_speed` (cm/s).

## 4. Lab (`tools/lab/melee_prompt_lab.sh`, SS4 Forum)

- **Pompeii** has no meleeable combat enemy: its Beheaded are decorative, and Skeleton Soldiers are not in the
  selection script.
- **On Forum**, Processed held 0.9 m in front (facing the player, level) bring the prompt up in VR. Right-hand swings
  kill them: `melee: prompted kill of … by a right-hand swing at 27.0 m/s (hand path)`, twice per run.
- No canned melee started.
