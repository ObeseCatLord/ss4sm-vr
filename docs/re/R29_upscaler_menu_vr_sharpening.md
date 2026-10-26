# R29: sharpening slider, VR render resolution row, foliage under DLSS (Sam4.exe 616154, SamSM.exe 628492)

Sources:
- Ghidra decompiles from a private scratch copy of `lab/ghidra_r19` (`tools/ghidra/DecompileList.java`), kept privately
  in `lab/r29/s1.c`, `s2.c`.
- capstone disassembly (`tools/disasm.py`), `strrefs.py`, `callers.py`, and the masked-signature ports in
  `tools/port_symbols.py` (all four exes).
- Lab runs on the isolated lab copy (§6).

Addresses are Sam4.exe VAs unless marked **SM**. Notation as in R28.

## 1. Requests
1. A sharpening slider for the mod's upscalers, 0..100 %, default 25 %, independent of the upscaler choice.
2. In VR, the "Max 3D rendering Mpix" choices are replaced by a percentage of the headset's full eye resolution (the
   MPix values are monitor sizes).
3. Foliage shimmer with DLSS (§5).

## 2. CSliderWidget and the widget helpers

The engine has no slider in `CMSGraphicsOptions`. Its XML menu builder `0x140856630` (`<widget ... "SliderWidget">`,
attributes `cvar`, `name`, `tooltip`, `showvalue`, range and step) builds one. The mod copies that sequence:

```
w = engine_alloc(0x1b8, *slider_type)          // slider_alloc_site 0x140856bab: mov ecx,0x1b8; ...; mov rdx,[rip+type] (+9);
CSliderWidget_ctor 0x14091f520(w, menu)        //   call engine_alloc (+0x13); ...; call slider_ctor (+0x23)   SM 0x1408569ab / 0x14091fcc0
widget_add_child 0x14015a9a0(w, panel)         // appends to panel+0x28 (count +0x30); parent handle at w+0x24
w+0x08 = ident("SliderWidget"...)              // set_name 0x1400d90f0 is `mov [rcx+8],rdx; ret`
widget_set_label 0x140907d10(w, label, 1)      // ETRS/DTRS/TTRS keys translated, other text verbatim (R28 s1.2)
widget_set_tooltip 0x14015ab70(w, menu, "%1", 0xabcd0009, text)   // variadic; creates the tooltip object on first use
slider_range 0x140920070(w, min, max, steps)   // +0x180 min, +0x184 max, +0x17c (max-min)/steps
wlink_bind 0x140158a50(w, cvar)
slider_showvalue 0x1409202d0(w, mode, 0)       // +0x1ac, +0x1b0
```

- **Value.** `slider_sync` 0x14091ff40 (the update) reads every link with `wlink_get` and parses the text as a float
  into +0x178, clamped to [min, max]. `slider_write` 0x140920220 (user input) stores the clamped value, fires
  `OnChange` and calls `wlink_set(link, text of the float)` for every link. So the slider goes through the same two
  hooked functions as the R28 multichoice.
- **The ctor** sets min 0, max 100, step 1, value 25 (`slider_write(w, 25, 0)` with no links yet) and copies the
  style of `menu` (`menu+0x1a8`, `+0x198`, `+0x90`, `+700`).
- **`menu`** in `gfxopt_init` is `CMenuScreen::menu(this)` 0x140148b70 (SM 0x140149830), `[[this+0xb8]+0x30]` in both
  builds. **`panel`** is the parent of the Rescaling Filter row: `handle_entity(*(int*)(filter+0x24))`
  (`handle_entity` 0x141112c50 resolves any object handle).
- **`wlink_snapshot`** 0x14015ecd0, run by `wlink_bind`, calls `cvar_get_str` 0x14112ad30 directly. For an undeclared
  name that logs "Cannot get values for cvar %1. The variable is not declared." The mod therefore binds through
  `ren_iRescalingFilter` and renames the link right after: `cstring_assign(link+0, "ss4smvr_upscaler_sharpness")`.
- **SM.** The same field offsets (ctor stores at +0x17c/+0x180/+0x184/+0x150/+0x154, `slider_range` 0x140920810
  byte-identical, set_name `mov [rcx+8],rdx`).

Ported symbols (signatures.json): `slider_alloc_site` (masked 48 bytes), `slider_ctor` and `widget_set_label` (the 2nd
and 6th call after that site), `widget_add_child` and `widget_set_tooltip` (64 bytes). All four exes resolve.

The runtime gate (gfx_menu `prepare`) checks the site bytes (`B9 B8 01 00 00` at +0, `48 8B 15` at +9) and that +0x13
and +0x23 call `engine_alloc` and `slider_ctor`. On a mismatch the slider is not made; sharpening still works from
the saved value or option `upscale_sharpness`.

## 3. Sharpening

- DLSS 2.2.9+ ignores its sharpness parameter (preset K has none), and FSR 3.1's own RCAS only runs inside FSR. The mod
  runs **AMD RCAS** (FSR 1 / FSR 3.1's sharpener, MIT) itself after either backend: `src/up/shaders/rcas.comp`, with
  its noise removal, on the upscaled HDR image before post-processing. Each tap goes through `c/(1+max(c))` and the
  result back through the inverse, because RCAS expects [0, 1].
- The strength maps linearly: slider 0..100 % → RCAS `con` 0..1 (1 is RCAS's maximum, FSR 3.1's sharpness 1.0).
  0 % skips the pass. FSR 3.1's internal sharpening is always off now, so both backends sharpen the same way.
- **Slider link.** `ss4smvr_upscaler_sharpness`:
  - `wlink_get` constructs the out string with the mod's value ("25");
  - `wlink_set` parses it and saves `sharpness=<n>`;
  - `wlink_reset` (Default) sets 25.
  - No Lua runs and no cvar exists for it.

## 4. VR render resolution

- **Mpix row.** Found among the panel's children: a multichoice (same vtable as the filter row) with a link named
  `ren_fMax3DRenderingMPix` and 8 numeric options without aliases.
- **Conversion.** In VR its options are rewritten in place (`cstring_assign` on option +0x00 label and +0x10 value) to
  33/50/58/67/75/83/90/100 % with values `ss4smvr:pct<N>`. It is relabelled "VR Render Resolution" with a tooltip, and
  its label cache is reset (+0x1d8 = −1).
- **Link hooks:**
  - `wlink_get` reports the mod's value;
  - `wlink_set` of a mod value sets the percentage and is swallowed;
  - any other value (KeepSettings' revert of the snapshot) is the engine's own write of the cvar's own value and goes
    through;
  - `wlink_reset` sets 100 % and lets the engine write its default, as stock "Default" does.
- **Render size.** Per eye, `round(eye·pct/100)` per axis, where "eye" is the eye target (OpenXR recommended ×
  `vr_fRenderingPixelRatio`).
  - With DLSS/FSR below 100 %: phase B (R25) at that size; DLSS quality mode from the ratio as in R28.
  - Without an upscaler: the RenderView limiter call of an eye target returns that size, and the HDR allocation's
    call returns its input. The engine then renders its 3D canvas smaller and rescales it with the user's Rescaling
    Filter, as it does for a pixel ratio.
  - At 100 % the limiter returns the eye size: the user's desktop MPix cap no longer reaches the eyes.
- **Stereo path.**
  - `vr_iUsingSinglePassStereo` is recomputed every frame from `vr_bSinglePrepareRendering` (R2), so the mod switches
    it at device slot 0x030 (frame start).
  - Single pass only without an upscaler at 100 %; two-pass otherwise. Single pass's RenderView has no device eye,
    so neither the upscalers nor the percentage can apply there.
  - The value is set through `se4::setIndex`, which restores it before the engine saves cvars.
  - Option `single_pass=1` forces single pass and turns VR menu mode off; `single_pass=0` forces two-pass.
- **Backend switch.** The VR backend switch from the menu also happens at slot 0x030 (both eye histories reset).
- **State file.** `ss4smvr/upscaler_menu.txt` gains `vr_upscaler=`, `vr_render=`, `sharpness=`. The desktop key
  `upscaler=` is unchanged, so desktop and VR keep separate choices.

## 5. Foliage shimmer under DLSS

- **Vegetation sway is a vertex animation** in `VegetationShader.vsh`:
  - tree bend from `g_vTreeSwayStrengths`;
  - branch and edge sway from `SmoothTriangleWave((phase + c_fCurrentTime)·freq·c_fBranchSwaySpeed)`, with phases from
    the object's world position, a branch weight and the vertex position.
  - The cvar `sha_iAllowTreeSwaying` (INDEX, flags 0x10006 = saved) switches it.
- **Motion vectors.** The mod's motion vectors come from depth and camera motion only (`motion.comp`). Swaying leaves
  and bushes therefore move without motion vectors, so DLSS rejects their history and shows the jittered current
  frame: shimmer.
- **The engine's own MV path doesn't fix sway.**
  - The engine has a motion-vector permutation, `GENERATE_MVECS=1`. The depth/shadow pixel shader writes
    `(depth, velocity.xy, 1)` from `vClipSpacePos` and `vPreviousClipSpacePos`.
  - But the vegetation vertex shader's previous position is `mul(vObjVtxPos, mMVPPrevLeft)` with the **current**
    frame's swayed position, so even that path has no sway motion.
  - No cvar enabling it was found.
- **The cvar's six reads** (all four exes; `cvar_readers.py` also lists a seventh in SM, a `cmp [rip+X], r12d` whose
  target is the variable 4 bytes before the cvar, so not a read of it):
  - Tree bend per instance (`0x140232240`, from `sp_fTreeSwaySpeed` × time and `sp_fTreeSwayStrength`).
  - The branch / edge strengths and speed copied into the instance constants (`0x140231f20`: shader params +0x3a8
    `sp_fBranchSwayStrength`, +0x3c0 `sp_fEdgeSwayStrength`, +0x390 `sp_fBranchSwaySpeed`; the CShaderArgValFLOAT
    members are at +0x398/+0x3b0/+0x380, value at +0x10).
  - The instanced-vegetation sway multiplier (`0x1402225a0`, 1.0 or 0.0).
  - Two more in the forest/instancing setup (`0x14021c7c3`).
  - Semantics: 0 none, 1 only where the caller's flag is 0, 2 all.
- **The mod's control (`src/se4/foliage.cpp`):**
  - At startup it scans `.text` for rip-relative `mov r32` / `cmp r/m32, r32` / `cmp r/m32, imm8` reads whose
    displacement lands on the cvar: 6 in each exe (runtime: "6 of 6" in Sam4 and SamSM).
  - It points them at a mod int allocated near the image.
  - Every frame (flat: player-views entry; VR: slot 0x030) that int is the cvar's value, or `min(cvar, mode)` while
    an upscaler runs. The cvar is never written.
- **Menu.** A "Foliage Sway (DLSS / FSR)" multichoice row: Game Setting / Off (default Off, §6), saved as
  `foliage_sway=game|off`; option `upscale_foliage_sway` (-1 / 0, or a level 1..2 cap) overrides it.
  - It is built like the filter row, from `gfxopt_init`'s own construction of that row: at +0xd0c,
    `mov rdx,[rip+type]; mov ecx,0x1f0; call engine_alloc; …; call CMultichoiceWidget_ctor`; at +0xda4, the option
    collection's `mov ecx,0x18 … call ctor`. The offsets and bytes are identical in all four exes.
  - Its link is renamed after binding, as for the slider.
- **Lab measurements:** §6.

## 6. Lab (2026-10-26, SS4 lab copy, flat 1280x720, Pompeii after the intro, still camera)
`tools/lab/flat_stability.sh` starts the level, waits for the health HUD (the intro cutscene cannot be skipped in
flat), settles 10 s and captures 10 frames. `image_stats.py` gives the mean frame-to-frame difference over the
upper-centre region (trees and bushes; no HUD or weapon).

Pitfalls found on the way:
- The lab copy's quality preset sets `sha_iAllowTreeSwaying` to 1, and a command-line `+sha_iAllowTreeSwaying` does
  not survive the preset. The user's `.ini` has 2. The lab option `lab_foliage_sway=N` makes the mod's redirect
  report N as the game's value.
- A fixed 150 s settle sometimes still caught the cutscene, or a moving camera (the walls changed too).

| config (DLSS = DLAA at 720p, no sharpening) | temporal diff |
|---|---|
| no AA, sway 2 | 1.79 |
| no AA, sway 0 | 0.42 |
| DLAA, sway 2 | 0.89 |
| DLAA, sway 0 (`upscale_foliage_sway=0`, the mod's redirect) | 0.61 |
| DLAA, sway 1 (the lab preset) | 0.87 |
| DLAA, sway 1, preset J | 0.83 |
| DLAA, sway 1, `upscale_lod_bias=0` | 0.81 |
| DLAA, sway 1, sharpening 25 % | 0.89 |

- **Per-pixel deviation maps** (`lab/runs/foliage5`): with sway the whole bush changes from frame to frame under DLAA;
  with the redirect at 0 it is as still as the wall behind it.
- **Sway is the shimmer.** Level 1 and level 2 behave the same on these bushes.
- **Sharpening at 25 %** adds a little; **preset J** and **LOD bias 0** change little at DLAA.
- **Default.** The mod's "Foliage Sway (DLSS / FSR)" row therefore defaults to Off while an upscaler runs. Game
  Setting brings the game's sway back.

### 6.1 Menu, desktop (SS4 lab copy, flat)
- **Rows.** Options > Graphics shows "Upscaler Sharpening" (slider, 25.00) and "Foliage Sway (DLSS / FSR)" (Off)
  right after FidelityFX CAS, in the stock style.
- **Clicks.** Clicking the slider at 60 % logs `menu: sharpening 60%` and saves `sharpness=60`. The sway popup lists
  Game Setting / Off; picking one saves `foliage_sway=game`.
- **Default (F2) → Yes** resets sharpening to 25 % and sway to Off; the filter turns the upscaler off as before.
- **No "Cannot get values for cvar" lines** in the game log once `wlink_snapshot` skips the mod rows.

### 6.2 Menu, VR (SS4 lab copy, simulated Monado, pointer clicks)
- **Startup** from `vr_upscaler=dlss`, `vr_render=67`:
  - two-pass from the first frame;
  - `phase B on: DLSS at 67% of the eye size`, eye 1344x1512 → 900x1013, DLSS quality mode 2;
  - `RCAS sharpening 25% (DLSS)`, `foliage: sway 0`;
  - no history resets with a still head.
- **Graphics menu in VR:**
  - the Mpix row reads "VR Render Resolution: 67%", with a popup 33 %..100 %;
  - the slider and sway row are present, and the slider tooltip shows.
- **Runtime changes:**
  - 50 % → 672x756, DLSS performance mode.
  - FidelityFX CAS at 100 % → `VR upscaler DLSS -> off`, `VR stereo single pass`; the stereo image is correct.
  - DLSS again → `VR stereo two-pass`, DLAA (quality 5).
  - CAS at 67 % → `VR stereo two-pass (off, 67%)`, `VR limiter: eye N 1344x1512 -> 900x1013 (67%, off)`: the
    engine's own resized canvas and rescale; the image is correct.
- **Single pass.** RenderView runs with device eye 0 at the eye size: an eye-size target there keeps its size (100 %
  only). In the lab the engine's MPix cvar reads 0 in VR, so the override changed nothing there.
- **VR suites** (VR menu mode on by default): SS4 21 passed, SM 21 passed, 0 failed.

### 6.3 SM (lab copy, flat)
The same rows in SM's Options > Graphics. The slider saves `sharpness=60`; there are no "Cannot get values" lines;
foliage "6 of 6 reads redirected".
