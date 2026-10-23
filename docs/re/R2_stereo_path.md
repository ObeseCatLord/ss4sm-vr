# Gate R2 — stereo path (in progress)

## Static evidence (Sam4.exe 616154)
`vr_iUsingSinglePassStereo` (INDEX at 0x1422aacec) has 21 code readers (`tools/pe_fninfo.py`):
- 0x1401dc578: GPU/affinity setup together with `vr_bEnableGPUAffinity`, `gfx_iForceGPUsCount`,
  `gfx_ctConcurrentGPUs` and the mixed-reality output cvars.
- 0x140b6b498: a script/console wrapper.
- 0x140f3fbb8, 0x140f403f0, 0x140f407b0: generic renderer (occlusion queries, multi-GPU).
- Three near-identical families, one per graphics backend — 0x14138dc80/0x14138e837/0x14138ee60,
  0x1413ba469/0x1413babb0/0x1413bb60d, 0x1413d405d/0x1413d4830 — in the draw-batching paths next to
  `gfx_iBatchPrimitivesMin/Max` and `gfx_bBatchLimitIndexed`. Each selects a shader permutation:
  `index = base + (flag ? 0x400 : 0)`, with `vr_iUsingSinglePassStereo > 0` forcing `index = base`.

So stereo-aware shader selection still exists in every backend's draw path. Which backend is which,
and whether instance doubling or viewport broadcast happens, still needs the Ghidra decompile and a
RenderDoc capture with the cvar forced (requires the Proton prefix, i.e. a first launch).
