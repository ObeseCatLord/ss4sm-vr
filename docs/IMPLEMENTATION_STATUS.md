# Implementation status

## Phase 0 — recon and RE gates
- [x] Repo, AGENTS.md, plan (`docs/PLAN.md`)
- [x] Fingerprints for Sam4, Sam4_Unrestricted, SamSM, SamSM_Unrestricted (`docs/re/builds/fingerprints.json`)
- [x] Registration-table harvest (`tools/harvest_symbols.py`): ~1490 cvars, ~3900–3950 script functions per exe
- [~] R1 device seam: outcome B established; full slot semantics in progress (`docs/re/R1_device_seam.md`)
- [~] R2 stereo path: stereo shader selection live in all three backends (static); runtime capture pending (`docs/re/R2_stereo_path.md`)
- [ ] R3 live VR gameplay
- [~] R4 content mounting: unsigned workshop .gro mount in Unrestricted (`docs/re/R4_content.md`)
- [x] R5 graphics interop: PASS on Proton + simulated Monado (`docs/re/R5_graphics_interop.md`)
- [x] First flat launch of each game (prefixes 257420, 1792250 on GE-Proton11-9); SS4 on Vulkan verified

## Phase 1 — vertical slice
- [x] msimg32 proxy + fingerprint-gated payload load in SS4 (Proton); no startup stall
- [x] Game Vulkan instance/device created through XR_KHR_vulkan_enable2 (multiview feature added)
- [x] XR session on the game's graphics queue (family 0/0), queue access serialised with the runtime
- [x] Milestone "mirror": presented game image blitted into a world-locked quad layer
      (simulated Monado, `docs/evidence/2026-10-23_mirror_quad_monado_sim.png`)
- [ ] Same on Siberian Mayhem; on real Beyond/Monado; on SteamVR
- [ ] Device seam (`COpenXRDevice` at the VR device global) — needs R1 slot semantics
- [ ] Engine stereo rendering into the projection layer (R2)
