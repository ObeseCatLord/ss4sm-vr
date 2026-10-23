# SS4 + Siberian Mayhem OpenXR VR — agent instructions

Owner: ObeseCatLord. Plan: [docs/PLAN.md](docs/PLAN.md). Status: [docs/IMPLEMENTATION_STATUS.md](docs/IMPLEMENTATION_STATUS.md).

## Scope
6DoF VR for Serious Sam 4 (Sam4.exe, build 616154) and Siberian Mayhem (SamSM.exe, build 628492):
native dual wield, physical melee (inertia + swept collision), weapon wheel, gadget wheel with held/usable
gadgets, single-pass stereo, MP with networked full-body IK, mixed desktop+VR play (all players have the
mod), 2D menus/HUD on composition layers, vehicles driven by thumbsticks (always available) or, optionally, by
grabbing the vehicle model's own steering wheel/handlebar bones, projected native crosshairs (no laser beams). Teleport is excluded.
Targets: Proton + Monado/Envision (Bigscreen Beyond, Knuckles) first, SteamVR, native Windows.

## Rules
- No invented offsets, fields or functions. Every address comes from the engine's registration tables or a
  signature verified unique in all four exes, recorded in `docs/re/builds/<sha>.json`. Unknown means unknown.
- Fingerprint gate: an unrecognised exe leaves the payload inert and the game flat.
- Drive native state machines (commands, wheels, gadget modes, vehicle input, damage). Never duplicate
  gameplay state or run simulation/draw twice per frame for stereo. Gameplay mutation stays on the
  simulation thread; render threads read pose snapshots only.
- Never modify game files, `All.dat` or official `.gro` archives. Install adds files only, with a receipt.
- Never commit extracted Lua, shaders, models or other game assets. Private dumps live in `lab/` (gitignored).
- SS2 `src/common` (MIT) may be ported. eduke32_vr (GPL-2) is a design reference only.
- Shared engine code goes in `src/se4`; per-game differences only in `src/games/{ss4,sm}`.
- A phase is complete only when both SS4 and SM pass its verification.
