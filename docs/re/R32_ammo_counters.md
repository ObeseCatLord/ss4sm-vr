# R32: on-weapon ammo counters in VR (Sam4.exe 616154, SamSM.exe 628492)

Sources:
- Ghidra decompiles from a private scratch copy of `lab/ghidra_r19` (`lab/r31/a*.c`).
- Content listings of `Content/SeriousSam4/00_All.gro` (names only, nothing extracted into the repo).
- The Serious Sam Fusion 2017 install and the SS2 VR mod (`src/common/ammo_screen.hpp` there), as references.
- Lab runs on the isolated SS4 copy.

## 1. The engine's own VR counter (Fusion's design, still in SS4)

- **Engine side, all intact.** Every weapon step (`weapon_step` 0x1402de9a0 and 0x14076f510) ends with
  **0x1402d4b70**, which sets the weapon model's shader arguments:
  - `"100"`, `"10"`, `"1"`: the digit's offset along a digit strip, step 0.04248. Leading zeros are blank (offset 0),
    and the ones digit is always shown.
  - **`AmmoCounterColor`**: (0x26, 0xcd, 0xf7) normally. Below a tenth of the maximum it is red, alternating alpha
    0x40 / 0xff on a 0.5 s period.
  - The count and maximum come from weapon `vt+0xa90` and `vt+0xa98`.
- **Content side.** In Fusion, every `*_VR.bmf` weapon mesh has an `AmmoCounter` surface (UV map `Digits`) with the
  `Counter_1/10/100.mtr` materials: `DigitalDigits_CM.tex`, base offset U from `"1"`/`"10"`/`"100"`, base colour from
  `AmmoCounterColor`.
  - SS4 ships the texture and the three materials.
  - It ships `_VR` meshes **only for the minigun, the grenade launcher and the detonator**: `Minigun_VR.bmf` and
    `GrenadeLauncher_VR.bmf` have the `AmmoCounter` surface.
  - Their params set `wp_pmcVRModel` (+0xd8). The model getter 0x1402d7f90 uses it when
    `en_bOwnerPlayingInVR` (+0x1f0) is set, which is every player with CPlayerVRData, ours included.
  - So these two guns should show the engine's own counter in the mod's VR. **Not yet seen in the lab:** the lab
    player at the Pompeii start owns only the pistol, shotguns and knife (wheel map at 0.09 / 0.19 / 0.28 m).
- **Every other SS4 weapon** uses its normal model, which has no counter surface. Adding surfaces would mean new
  meshes, and the mod never touches game content.

## 2. The mod's counters (`src/se4/ammo_counter.cpp`, `src/vk/ammo_layer.cpp`, `src/core/ammo_counter.hpp`)

- **Values.** The same calls as the engine counter (`vt+0xa90` / `vt+0xa98`), read from the weapon step hook
  (`hands.cpp`) for each weapon in the local player's hands.
  - No counter when the maximum is ≤ 0: the pistol has infinite ammo (max −1), and the knife and other melee
    weapons have none.
  - Weapons whose params set `wp_pmcVRModel` keep the engine's counter (`ammo_counter_native=0` draws on them too).
- **Look.** The engine's rules (three digits, blank leading zeros, cyan, red blinking below 10 %), drawn as slanted
  seven-segment digits on a dark rounded panel, after the SS2 mod's ammo screens. Unlit segments sit just above the
  panel, so they do not read as an 8. The digits are drawn on the CPU (`core::ammo::rasterize`, unit-tested in
  `tests/ammo_counter_checks.cpp`).
- **Placement.**
  - The weapon's frame relative to the hand's grip is the engine's weapon placement (`vt+0xa8`) against the
    controller's world pose (`player_vr::controllerWorld`), converted from world units to metres.
  - The spot in that frame: default 8.5 cm up and 3 cm back of the grip, on the line of sight over the receiver.
    It is a 4.5 cm wide quad, tilted 40° back towards the eyes.
  - Per-weapon overrides by params file (`kSpots`: pistol, revolver, rocket launcher, cannon) plus
    `ammo_counter_up_cm`, `ammo_counter_back_cm` and `ammo_counter_size_cm`.
- **Rendering.**
  - One OpenXR quad swapchain (256x256, `R8G8B8A8_SRGB`): left hand in the top half, right hand in the bottom.
  - It is filled each frame from a host-visible staging ring (3 slots, one per mirror frame in flight) and submitted
    as one quad layer per visible hand in play space, with unpremultiplied source alpha.
  - The pose uses the grip at the predicted display time, the same as the scope layer.
- **Limits.**
  - A quad layer composites over the scene, so a hand or object in front does not hide the counter. The SS2 mod
    draws in the scene with a depth test.
  - The spots for weapons other than the double shotgun are not lab-checked yet.
- **Options:** `ammo_counter` (default 1), `ammo_counter_native`, `ammo_counter_size_cm`, `ammo_counter_up_cm`,
  `ammo_counter_back_cm`.

## 3. Lab (`tools/lab/ammo_counter_lab.sh`, SS4 Pompeii)

- **Pistol:** `no ammo counter (no ammo) (ammo 0 of -1)`.
- **Double shotgun:** `counter on the weapon (ammo 40 of 100)`. The counter sits on the receiver above the right
  hand. It read 40 and then 36 after the shots, the same as the HUD.
- **Bug found and fixed in the lab:** the mirror path releases the layer's image before it builds the layer list,
  so `layers()` keeps its own per-frame flag.
