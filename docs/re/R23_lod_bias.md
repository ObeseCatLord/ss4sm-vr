# R23: negative texture LOD bias while the upscaler is active (Sam4.exe 616154, SamSM.exe 628492)

Static analysis only. Sources: capstone disassembly of all four exes (`tools/disasm.py`, `callers.py`,
`cvar_readers.py`, `riprefs.py`), Ghidra decompiles from a private scratch copy of `lab/ghidra_r13` (functions
listed per section), and masked-signature ports to the other three builds. Nothing was run, and no game file, cvar,
`src/` file or lab file was changed. Addresses are Sam4.exe PE VAs unless marked. "gfx" = the engine graphics object
(`DAT_1422aa9b0`, the same object as `param_1` of the PP dispatch). "slot state" = the 8-byte sampler descriptor
described in §2.

## 0. Answers

1. **One reader feeds scene materials: `bind_texture` 0x140b80f70** (its load site 0x140b812c9 is the `.pdata` chunk
   listed as "0x140b8126a"). Every material, particle and model texture bind goes through it (99 direct callers). It
   quantises `tex_fLODBias` into byte 0 of the slot's 8-byte sampler state. **The terrain renderer** (0x140fa4fd0, reads
   `trn_*` cvars) binds its layer textures by handle through a second writer, **0x140f47d50** (load 0x140f47db2), which
   does the same thing. The other three readers do not feed draws (§1).
2. **No shader constant carries the bias.** It exists only in the sampler state. The Vulkan backend turns the slot state
   into a `VkSamplerCreateInfo` with `mipLodBias = (int8)byte0 × 0.12421875` (§2).
3. **Samplers are cached by the whole 8-byte slot state, bias byte included.** The global hash map is `0x1423b2508`
   (SM `0x14238f588`). A new state creates a `VkSampler` once, and later draws reuse it. Changing the bias **at bind time**
   therefore needs no invalidation: the new key gets its own sampler, and the unbiased ones stay valid for other passes.
   Changing it **at `vkCreateSampler` time** would be stuck in the cache: the key does not include the change.
4. **Recommendation: two 8-byte in-memory detours of the bias loads**, at 0x140b812c9 and 0x140f47db2. Each becomes
   `jmp stub`, and the stub does `movss reg,[tex_fLODBias]; addss reg,[g_extraBias]; jmp back` (§4). `g_extraBias` is a
   mod float that is −1.0 only while an eye's scene is executing with the upscaler on. It is set at the entry of
   execute-view 0x140bc0d20, if the device eye is 1 or 2, and cleared in the existing PP-dispatch detour and at
   execute-view exit.
   - The cvar is never written.
   - The engine's own exemptions stay intact: textures with flag 0x10 and the zero-bias pass 0x140bc1610.
   - The result goes through the engine's ±15.9 clamp and quantisation.
   - It is backend-independent, so it covers the D3D11/D3D12 decoders too.
   - The patch sites are unique by signature in all four exes.
5. **A `vkCreateSampler` hook (§5) is the fallback**, useful for a quick lab A/B test. Its bias would be constant for the
   whole session and could not be told apart per pass. The engine's per-texture "no bias" flag (0x10) is invisible at
   the Vulkan level, so UI, menu, font, HUD and scope samplers would be biased too.

Confidence: high for the bind/quantise/decode/cache chain and the patch bytes (read in all four builds). High that only
0x140b80f70 and 0x140f47d50 put the cvar into sampler state. Medium for the meaning of texture flag 0x10 and of pass
0x140bc1610 (behaviour read, purpose inferred). Medium that every per-eye scene draw executes inside 0x140bc0d20 before
the PP dispatch. R13 and the existing `hookedPp` device-eye read support this, but it has not been checked in the lab.

## 1. The five `tex_fLODBias` readers (0x1422b3f70, flags 0x10006)

| reader (fn) | sites | what it does | relevant? |
|---|---|---|---|
| 0x140b80e00 "filter normalise" | load 0x140b80f32, **store** 0x140b80f5d | Derives `tex_iFiltering`/`tex_iAnisotropy` and writes `tex_fLODBias = clamp(v, -15.9, 15.9)` (constants 0x1415e0834 / 0x1415e0830) back into the cvar. It is called from the stats dump and from 0x140b5ca90. | no. It is also the reason the mod must not write the cvar: the engine itself rewrites it, and it is persistent. |
| **0x140b80f70 `bind_texture`** | **load 0x140b812c9** | Per-slot sampler state (§2). Textures with flag 0x10 get bias 0 and anisotropy 1. | **yes: all material textures** |
| 0x140b99130 (stats print) | load 0x140b9977b | Prints "LOD bias: %.2f" / "none". | no |
| 0x140bc1610 "zero-bias pass" | load 0x140bc16b2, stores 0x140bc16c9 / 0x140bc182c | When `DAT_1422d4d54 == 0`, it **temporarily** sets `tex_fLODBias = 0`, `tex_iFiltering = 0x15` and `tex_iAnisotropy = 1`. Then it runs the list at `DAT_1422d4f10` (0x140dac140) and 0x140d52a30 and restores all three. It is called after RenderView 0x140bc1cd0 and before execute-view 0x140bc0d20 (e.g. 0x140bc33b0). | Indirect: the engine wants these binds unbiased. The recommended stub must therefore be gated outside this pass (it is, §4.2). |
| **0x140f47d50 `bind_by_handle`** | **load 0x140f47db2** | Same slot-state write for a raw texture handle (`wrap = param_5`). Its only caller is 0x140fa4fd0, the terrain renderer (`trn_iRenderCrumbs`, `trn_fGrassCrumbsGap`), which binds pixel slots 1, 4, 7 and 9. | **yes: terrain layers** |

## 2. Slot state and the Vulkan sampler (decompiles 0x140b80f70, 0x1413922c0, 0x1413935f0, 0x1413945f0)

`bind_texture(gfx, tex, stage, slot, flag, forcedTex)` (and `bind_by_handle`) write:

- `gfx + 0x380 + stage*0x80 + slot*4`: the texture id (bit 31 = `flag`).
- `gfx + 0x500 + stage*0x80 + slot*8`: the **8-byte slot state** `d[0..7]`:

| byte | meaning (from the writers and the Vulkan decoder) |
|---|---|
| d[0] | **LOD bias**, `(int8)round(clamp(bias, -15.9, 15.9) * 8.050315)` (0x1415e082c). |
| d[1] | border colour select; it is only part of the key when an address mode is 0x52 (border). |
| d[2], d[3], d[4] | address mode U, V, W (table 0x142099b08); U and V come from tex vt+0x150/+0x158. |
| d[5] | filter index (base `DAT_14224cf60` from `tex_iFiltering`, adjusted by tex vt+0x190). It indexes mag/min (0x142099b90/94) and mipmapMode (0x142099c08). **0xd/0xe = compare (shadow) filters.** |
| d[6] | max anisotropy (tex_iAnisotropy, `tex_bBoostAnisoVRS` may double it; flag-0x10 textures get 1). |
| d[7] | 0xff = no max LOD override. |

The bias value written, from the decompile of 0x140b80f70:

```c
fVar17 = tex_fLODBias; aniso = tex_iAnisotropy;
if (tex->vt[0xa8]() & 0x10) { aniso = 1; fVar17 = 0.0; }      // engine-exempt textures
...
d[0] = (char)ROUND(clamp(fVar17, -15.9, 15.9) * 8.050315);
```

When a bind falls back to a default texture, all of d[] is set to defaults with d[0] = 0.

**Vulkan backend:** the per-stage state flush runs at draw time. 0x1413945f0 handles stage 0 (`gfx+0x500`),
0x1413922c0 stage 1 (`+0x580`) and 0x1413935f0 stage 2 (`+0x600`); they are called from 0x1413901c0 and
0x14138bf09/0x14138c1f0. For each bound slot:

1. key = the 8 bytes (d[1] cleared unless border). If the key equals the slot's last key (`+0x1e0`), the last
   `VkSampler` (`+0x360`) is reused.
2. Otherwise `0x1413955e0(&map 0x1423b2508, key)` looks the key up. On a miss the backend builds a `VkSamplerCreateInfo`
   (sType 0x1f) and calls `vkCreateSampler` through the pointer **0x1423b02f0** (SM **0x14238d220**). The pointer is
   loaded with `vkGetDeviceProcAddr(dev, "vkCreateSampler")` in 0x141367cf0. The result is inserted with
   0x141391e60 together with the frame number (`DAT_14224cf30`).

   The fields it fills:
   - `mipLodBias = clamp((int8)d[0] * 0.12421875, ±15.99)` (0.12421875 at 0x142099fb0).
   - `anisotropyEnable = d[6] > 1`, with `maxAnisotropy = min(d[6], gfx_iMaxTextureAnisotropy)`.
   - `compareEnable = (d[5] == 0xd || d[5] == 0xe)`, with `compareOp` 3 (LEQUAL) or 6 (GEQUAL) when
     `DAT_14224cdc8` (reversed depth) is set.
   - `minLod = 0`. `maxLod = 0` when `d[5] < 4` (non-mip filters) or for compare filters, otherwise 16 (or d[7] if it
     is not negative).
   - `borderColor` 2 or 4 for border address modes.
3. Cache maintenance:
   - 0x141384240 sorts the map and resets the use stamps (garbage collection).
   - 0x14137ea20 clears it.
   - 0x141397520 and 0x1415597c0 tear it down.
   - The only `vkDestroySampler` user (pointer 0x1423b02f8) is the deferred-destroy path 0x1413673d0.
   - `DAT_1422ab2ec & 2` (a debug switch) forces a map lookup on every draw.

Only these three functions call `vkCreateSampler` in the engine. The six other users of the 0.12421875 constant
(0x1413b5210, 0x1413b6060, 0x1413b6ce0, 0x1413cfc10, 0x1413d0650 and 0x1413d0f30) do not call Vulkan. They are probably
the D3D11/D3D12 backends' decoders of the same slot state (not checked further).

Consequences:

- A bias added to d[0] before the flush changes the cache key, so biased and unbiased samplers coexist and each is
  created once.
- At most the slot states used in eye scenes are duplicated, a few hundred samplers, well below
  `maxSamplerAllocationCount` (4000 or more).
- Quantisation: −1.0 → d[0] = −8 → `mipLodBias` −0.99375.

## 3. Options considered

| option | verdict |
|---|---|
| Write `tex_fLODBias` | Forbidden: it is persistent (0x10006), 0x140b80e00 writes it back clamped, and the game saves it to the user's settings. |
| **Detour the two bias loads (§4)** | **Recommended.** Per-pass gating, the engine's exemptions are kept, the cache keys are correct, it works with any backend, and there are only 2×8 bytes of code plus one MinHook. |
| MinHook `bind_texture` and adjust d[0] after it returns | It works, but it has to repeat the flag-0x10 and default-texture checks to avoid biasing exempt textures, and it costs a full extra call frame on the hottest bind path. The load detour sits exactly on the non-exempt branch (0x140b812c2 `je` → 0x140b812c9; the exempt branch is `xorps xmm6,xmm6; jmp 0x140b812d1`). |
| `vkCreateSampler` hook (§5) | Fallback or lab A/B only. |
| Shader constant | None exists: no shader-constant path reads `tex_fLODBias` (all five readers are listed in §1). |

## 4. Recommended implementation

### 4.1 Patch sites (verified unique in all four exes; no branch targets inside the 8 bytes)

Signature 1 (`bind_texture`): `74 05 0F 57 F6 EB 08 | F3 0F 10 35 <rel32 → tex_fLODBias>`. The patch is applied at
offset +7, over the `movss`. Resume = site + 8. In Sam4 the resume address is also the target of the exempt branch's
`jmp`, so it must stay an instruction boundary (it does).

Signature 2 (`bind_by_handle`): `48 8D 53 0A | F3 0F 10 05 <rel32 → tex_fLODBias>`. The patch is applied at offset +4.
Resume = site + 8 (`0F B6 05 …`, movzx tex_iAnisotropy).

| build | `tex_fLODBias` | site 1 (xmm6) | bytes | site 2 (xmm0) | bytes |
|---|---|---|---|---|---|
| Sam4 | 0x1422b3f70 | 0x140b812c9 | `F3 0F 10 35 9F 2C 73 01` | 0x140f47db2 | `F3 0F 10 05 B6 C1 36 01` |
| Sam4_Unrestricted | 0x1422b3f70 | 0x140b83c79 | `F3 0F 10 35 EF 02 73 01` | 0x140f4a942 | `F3 0F 10 05 26 96 36 01` |
| SamSM | 0x142291130 | 0x140b817a9 | `F3 0F 10 35 7F F9 70 01` | 0x140f48b22 | `F3 0F 10 05 06 86 34 01` |
| SamSM_Unrestricted | 0x142290130 | 0x140b84149 | `F3 0F 10 35 DF BF 70 01` | 0x140f4b6a2 | `F3 0F 10 05 86 4A 34 01` |

Patch each site with `se4::patch::writeCode(site, expect8, {E9 rel32(stub), 90, 90, 90}, 8)`. The stubs live in one
`se4::patch::allocNear(site, 64)` block, with `g_extraBias` (a 4-byte aligned float) inside the same block so that
rip-relative operands reach it:

```
stub1:  F3 0F 10 35 <disp32 → tex_fLODBias>   movss xmm6, [tex_fLODBias]
        F3 0F 58 35 <disp32 → g_extraBias>    addss xmm6, [g_extraBias]
        E9 <rel32 → site1+8>                  jmp resume1
stub2:  F3 0F 10 05 <disp32 → tex_fLODBias>   movss xmm0, [tex_fLODBias]
        F3 0F 58 05 <disp32 → g_extraBias>    addss xmm0, [g_extraBias]
        E9 <rel32 → site2+8>                  jmp resume2
g_extraBias: dd 0.0
```

- **Registers and flags:** no flags are live across either site. In site 1 the next flag writer is `test` at
  0x140b812f1. `addss` writes no flags.
- **Clamp and quantisation:** the engine's ±15.9 clamp (`minss`/`maxss` at 0x140b812fe/0x140b8131a, and in 0x140f47dc7)
  and the quantisation run after the stub, so the sum is clamped and quantised as usual.
- **Adding to the user's own bias:** the user's bias is kept and the mod's is added to it.
- **Applying the patch:** the existing patches set the order. Apply this one after `identifyBuild` (the in-memory .text
  CRC gate), and before rendering starts.

Recording rules (AGENTS): `tex_fLODBias` comes from the harvested cvar table. Both signatures, the resume offsets and the
expected bytes should be recorded in `docs/re/builds/signatures.json`; this report only lists them.

### 4.2 Gating `g_extraBias` (scene of eye 1/2 only, upscaler actually running)

The bias is read at bind time on the thread that executes the render commands, and the Vulkan flush reads the slot
state at the next draw on the same thread. So a plain global set around the eye's scene execution is enough:

- **New MinHook on execute-view 0x140bc0d20** (Sam4_U 0x140bc3900, SM 0x140bc1410, SM_U 0x140bc3fe0; masked-signature
  port, which also reproduces `pp_dispatch` for all four builds as listed in manual.json).
  - On entry: read the device eye `*(int*)(*vr_device_global + 0x18)`, as `hookedPp` does. If the eye is 1 or 2 and
    the upscaler will evaluate (`g_evaluate && g_evaluateOn && g_jitterOn && up::active() != Off`), set
    `g_extraBias = up::mipBias()`. That is `log2(renderW/outputW) − 1`, which is −1.0 for DLAA and FSR native AA; compute
    it from `g_color.w` and the eye output width once Phase B scaling exists.
  - Otherwise set it to 0.
  - Call the original, then set it to 0.
- **In `hookedPp`**: `g_extraBias = 0` before `realPp`. The post-processing, the rescale (`gfx_copy`/0x140bbf2c0) and
  anything that executes after PP in the same view are unbiased.
- **Excluded automatically:**
  - The zero-bias pass 0x140bc1610 runs outside execute-view, so its binds stay at the engine's 0.
  - The menu/cinema view, the desktop view and the scope (eye 3) fail the eye test.
  - Flag-0x10 textures skip the patched instruction.
- **Optional lab switch:** `upscale_lod_bias` in hundredths (default −100; 0 disables), next to the other lab switches in
  integrate.cpp.

### 4.3 Risks

- **Shimmer:** a negative bias without a temporal resolve aliases. The bias is therefore tied to the same condition as
  the jitter and the evaluation. With `upscale_evaluate=0` or a failed evaluate (`g_evaluate` cleared) it must be 0.
  The cache needs no flush because the key changes.
- **UI blur or aliasing:** not affected. The HUD, the menus and the end-of-frame 2D lists are bound outside the gated
  window, and the engine already forces their bias to 0 in 0x140bc1610.
  - **Lab check:** HUD elements that execute inside the eye's execute-view *before* PP would be biased. They are drawn
    after the upscaler, but a −1 bias on a texture shown at about 1:1 only has an effect when it is minified.
  - Count binds in the window, or log `d[0]` for the HUD's textures, in the lab.
- **Nested views:** if one execute-view runs more than one view's command lists (`DAT_1422d4f00[]` executed in reverse,
  then `DAT_1422d4d18`), a view that comes after the first PP runs unbiased. That is safe: it only loses the
  sharpening. If the lab shows a second eye view inside the same call, re-arm per command list in a detour of
  0x140bbef50.
- **Shadow, compare and render-target samplers:** compare filters get `maxLod = 0`, and render targets have one mip, so
  the bias has no effect. The only exception is the mag/min filter switch at λ≈0 when the two filters differ, which
  does not matter for these targets.
- **Particles and decals:** these are biased too, as NVIDIA recommends for all scene textures. If a noisy effect
  shimmers, exclude its callers.
- **Sampler cache growth:** about 2× the eye-scene slot states, created once. The engine's GC (0x141384240) handles
  them.
- **Threading:** the hooks and the binds run on the render-command thread (R13 §4.1: commands execute synchronously
  inside 0x140bc0d20). Use an aligned 32-bit store; no lock is needed.

## 5. Fallback: `vkCreateSampler` in the `src/vk` dispatch

Return a hook for `"vkCreateSampler"` from `hookGDPA`. The engine fetches it once per device in 0x141367cf0.

- **When to change the bias:** add the bias only when all of these hold:
  - the caller is the game module (`_ReturnAddress()` in Sam4/SamSM; the engine's call sites are 0x1413929c8,
    0x141393cf8 and 0x141394cf8, so NGX/FSR/mod samplers are left alone);
  - `!ci->compareEnable`;
  - `ci->maxLod > ci->minLod` (excludes the non-mip filters, `d[5] < 4`);
  - `ci->mipmapMode == VK_SAMPLER_MIPMAP_MODE_LINEAR` (optional);
  - the upscaler is configured for the session.
- **How:** copy the create info and set `mipLodBias = clamp(ci->mipLodBias + extra, ±limits.maxSamplerLodBias)`.
- **Why it is only a fallback:**
  - The key in map 0x1423b2508 does not include the change, so the bias is fixed for the lifetime of each sampler. It
    cannot be per pass, and it cannot be undone if the upscaler stops evaluating.
  - It biases UI, menu, font, HUD and scope textures too: flag-0x10 textures have the same filter and address bytes as
    other textures, and only bias 0 and anisotropy 1 distinguish them.
  - It is Vulkan only.
- **Where it is useful:** a five-minute lab A/B test of how much of the 25 % softness the bias recovers before the
  engine patch is written.

## 6. Lab verification (not run; SS4/SM lab runs need the user's go-ahead, see memory)

1. With the stub armed, log a sample of the d[0] values written per gfx stage during an eye's scene; expect −8 plus the
   user's bias. Also log the number of new `VkSampler`s created in the first seconds, which should stop growing.
2. Compare the static Pompeii capture (the deterministic readback from IMPLEMENTATION_STATUS "Next steps") against the
   6-frame supersampled reference. Fine corrugation should return. The bias does not fix geometric-edge softness, so
   remaining edge softness points back to jitter or MV handling.
3. Check that the HUD, menus, the scope PiP and the desktop mirror are unchanged, and that `upscaler=0` gives
   bit-identical frames with and without the patch (`g_extraBias` stays 0).
