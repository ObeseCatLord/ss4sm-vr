# R28: DLSS / FSR 3.1 in the game's GPU menu "Rescaling Filter" list (Sam4.exe 616154, SamSM.exe 628492)

Static analysis only. Sources:
- Ghidra decompiles from a private scratch copy of `lab/ghidra_r19` (`tools/ghidra/DecompileList.java`, `-noanalysis
  -readOnly`), Sam4 only. SM was checked by disassembly and by masked-signature ports (`tools/sigscan.py`,
  `tools/sig_at.py`, `tools/port_symbols.py`).
- capstone disassembly (`tools/disasm.py`), `strrefs.py`, `substrrefs.py`, `callers.py`, `cvar_readers.py`,
  `field_refs.py`, `vtable_slot.py`, `meta_members.py`, `meta_class.py`, `pe_fninfo.py`.

Nothing was run. No game file, cvar, user option, `.ini`, save, Steam userdata or `src/` file was touched. Addresses are
Sam4.exe VAs unless marked **SM**. Notation:
- "screen" = the `CMSGraphicsOptions` menu screen (Options > GPU / Graphics Options), base `CMenuScreen`.
- "widget" = `CWidget`; "link" = `CWidgetCvarLink`; "collection" = `CWidgetCvarOptionCollection`.
- `CString` = one `char*` (8 bytes) to an engine-allocated string; ctor `cstring_ctor` 0x1410bfac0, assign from
  `const char*` `cstring_assign` 0x1410bfe20, dtor `cstring_dtor` 0x1410bfce0 (all already in `manual.json`).
- Field names in *italics* are unreflected (offsets from code only, same in both builds).

## 0. Answers

1. **The menu and the widget.**
   - Screen: **`CMSGraphicsOptions`** (vtable 0x141b8dc20, SM 0x141b72470). The widgets are built in its init slot
     **vt+0x120 `gfxopt_init` 0x140872c60** (SM 0x140872bd0). The reflected member **`ms_pmwRescalingFilter` +0x1f0**
     is a **`CMultichoiceWidget`** (0x1f0 bytes, ctor 0x1409179b0, vtable 0x141bdc678; SM ctor 0x1409180a0, vtable
     0x141bc0f18). **`ms_pcwForceCAS` +0x1f8** is a separate **`CCheckboxWidget`** (ctor 0x140907480) bound to
     `ren_bForceCAS`, whose label is the same string "FidelityFX CAS".
   - The list is **not** filled with widget items. The widget gets a **cvar link** to `ren_iRescalingFilter`
     (`wlink_bind` 0x140158a50) and the link gets an **option collection** of (label, value-string) pairs, filled by
     `optcoll_add` 0x14015e3c0 six times:

     | index | label (translation key=default) | value written |
     |---|---|---|
     | 0 | `ETRSGPUMenu.RescalingFilter0=Point-sampling` | "0" |
     | 1 | `ETRSGPUMenu.RescalingFilter1=Bilinear` | "1" |
     | 2 | `ETRSGPUMenu.RescalingFilter2=Bicubic` | "2" |
     | 3 | `ETRSGPUMenu.RescalingFilter3=Sharpen-bicubic` | "3" |
     | 4 | `ETRSGPUMenu.RescalingFilter4=Lanczos` | "4" |
     | 5 | `ETRSGPUMenu.RescalingFilter5=FidelityFX CAS` | "5" |

     So **index = `ren_iRescalingFilter` value, 0..5**. Entry 5 is the CAS filter value 5 of the same cvar, **not**
     `ren_bForceCAS`. `ren_bForceCAS` is only the separate checkbox.
   - **Apply path.** Every selection (arrows, click, popup `CMSMultichoiceMenu`) ends in **`mc_setsel` 0x140918bf0**
     `(widget, index, noWrite=0)`. It sets *+0x178* = index, finds the option(s) with that label, and calls
     **`wlink_set` 0x14015ed30 `(link, &value)`**. That builds `"ren_iRescalingFilter=<value>"` and runs it as Lua through
     `lua_exec_string` 0x140b748a0. The change is **immediate**: there is no Apply step, no gfx restart and no
     KeepSettings dialog for this cvar. Then the script event `OnChange` fires (`CMSGraphicsOptions` has no
     `RescalingFilter_OnChange` handler).
   - **Read-back.** The widget re-reads the cvar on every update (vt+0x20 0x1409188e0) and when the popup opens
     (0x140918dd0), via **`mc_sync` 0x1409199e0**. That calls **`wlink_get` 0x14015eaf0** (the cvar's current text),
     then **`optcoll_find` 0x14015e5d0** (the option whose value matches), and returns the index and the cached display
     label. If no option matches, it returns −1 and the label `ETRSMenu.Custom=Custom`.
   - **Visibility.** The screen's per-step update **vt+0x178 `gfxopt_step` 0x1408760d0** shows the Rescaling Filter
     row and hides the ForceCAS checkbox **only when the selected resolution's megapixels exceed
     `ren_fMax3DRenderingMPix`** (or the cap is 0 = Unlimited). Otherwise it shows the ForceCAS checkbox instead. The
     flag is widget *+0x10* bit 0x20 = hidden.
2. **Cleanest hook (§2).** Four small MinHooks, all filtered by the link's cvar name, plus one helper call:
   1. **Post-hook `gfxopt_init`.** Find the `ren_iRescalingFilter` link on `*(this+0x1f0)` and call `optcoll_add`
      twice, with labels `"DLSS"` / `"FSR 3.1"` and **sentinel values** `"ss4smvr:dlss"` / `"ss4smvr:fsr31"`.
      - **Labels are plain text.** A label is translated only when it starts with `ETRS`/`DTRS`/`TTRS` and contains
        `=`. Anything else is shown verbatim. No string table is needed.
   2. **Hook `wlink_set`.** For this link, when the value is a sentinel: set the mod mode and **do not call the
      original**, so the cvar is never written. Any other value clears the mod mode and calls the original.
   3. **Hook `wlink_get`.** For this link with the mod mode on: return the sentinel text. `mc_sync` then selects index
      6/7 and shows the mod's label.
   4. **Post-hook `gfxopt_step`.** While the mod upscaler is available, clear bit 0x20 on `*(this+0x1f0)` so the row is
      always visible.
   5. Optional: hook **`wlink_reset` 0x14015eba0**, so that "Default" (reset to defaults) also clears the mod mode.

   `ren_iRescalingFilter` stays 0..5 at all times. The mod saves its own choice in its own file.
3. **Render-time consumers (§3).**
   - Only `execute_view` 0x140bc0d20 (read at 0x140bc0f0c, SM 0x140bc15fc) chooses the main-view rescale from the
     cvar:
     - canvas = output → mode 5 (CAS);
     - canvas/output > 2.0625 → mode 6;
     - filter 5 → CAS, or Lanczos when downscaling, or a filtered copy when the ratio is < ~0.485;
     - **0 → point copy; ≤ 1, including negative values → filtered copy; any value ≥ 2 → `rescale_pass(mode)`.**
   - In `rescale_pass` 0x140bbf2c0: 5 = CAS, 2/3 = Bicubic (3 sharpened), 4 = Lanczos, and **every other value (6, 7,
     …) = the "Average" shader** (box filter).
   - So an out-of-range saved value does **not crash**. It gives a poor box-filtered upscale and a menu showing
     "Custom".
   - The scope view 0x1407bf460 (SM 0x1407be9e0) saves the cvar, forces 1 and restores it (in memory only).
   - **Persistence:**
     - Both cvars are flags 0x10006, and bit 4 is the engine's **SAVED** flag. The `.ini` writer tests
       `flags & 4` (0x1410d7f98; SM 0x1410d87ae).
     - They are written to the project `.ini` by `prjSaveCVars` 0x140039f00 (SM 0x140039f80) and at shutdown
       (0x14003a4f8; SM 0x14003a588).
     - Per LAB_NOTES that file is `Steam/userdata/<id>/<app>/local/SeriousSam4.ini`; it was not opened here.
     - The file-init value of `ren_iRescalingFilter` is **5**.
4. **Resolution scale (§4).**
   - The GPU menu has **no `ren_fRenderingPixelRatio` widget**. Its only desktop resolution-scale control is **"Max 3D
     rendering Mpix"**, a multichoice bound to **`ren_fMax3DRenderingMPix`** (FLOAT, flags 0x40006, SAVED). Its
     options are 1.0 (720p), 1.5 (900p), 2.1 (1080p), 3.0, 3.7 (1440p), 5.8 (1800p), 8.3 (4K) and 0 = Unlimited.
   - **`ren_fRenderingPixelRatio`** (FLOAT 0x10006, file-init 1.0) is console/ini only. The VR menu row
     `ms_hmwRenderingRatioVR` drives `vr_fRenderingPixelRatio`, not this cvar.
   - Both are plain globals and can be read every frame. The clean source of the render size is the existing
     `mpix_limiter` hook (R25/R27): call the original and use its `outW/outH`. That is exactly "MPix cap ×
     pixel ratio" with the engine's own clamps and its 0.95 dead-band, so the menu's Max-MPix choice becomes the upscale
     quality.

Confidence:
- **[H]:** the widget construction, the option table and index→value mapping, `mc_setsel` → `wlink_set` → Lua, the
  read-back chain, the rescale mode mapping, the SAVED bit test, and the SM equivalence.
- **[M]:** that widget vt+0x20 runs every frame while the screen is shown (runtime check 2); the layout effect of
  un-hiding the row (check 4).

## 1. The GPU menu and the Rescaling Filter widget

### 1.1 Screen and members

`meta_members.py` (both builds identical): `CMSGraphicsOptions` (base `CMenuScreen`):

| member | offset | type (meta index) | widget built in `gfxopt_init` |
|---|---|---|---|
| `ms_hmwResolution` | +0x130 | 0x12 | resolution handle |
| `ms_astrSavedCvars` | +0x150 | 0x14 | "keep these settings" snapshot (res/fullscreen/API/monitor/MSAA/vsync/triple/MPix/vr ratio, filled at Apply by 0x1408757a0) |
| `ms_astrGfxRestartCvars` | +0x160 | 0x14 | cvars that need a gfx restart (`gfx_pixRes*`, `gfx_pixWin*`, `gfx_bFullScreen`, `gfx_bSimulatedFullScreen`, `gfx_strAPI`, `gfx_iFullscreenMonitor`, `gfx_iAntiAliasingSamples`, `gfx_iWaitVSyncs`, `gfx_bTrippleBuffering`, `vr_strAPI`) |
| `ms_pmwAPI` / `ms_pmwWindowMode` / `ms_pmwMonitor` | +0x1a0 / +0x1b0 / +0x1b8 | 0x17 | multichoice |
| `ms_hmwRenderingRatioVR` (+names/values) | +0x1c0 (+0x1c8/+0x1d8) | 0x12 | VR only (`vr_fRenderingPixelRatio`) |
| **`ms_pmwRescalingFilter`** | **+0x1f0** | 0x17 | **CMultichoiceWidget, name "RescalingFilter"** |
| **`ms_pcwForceCAS`** | **+0x1f8** | 0x18 | **CCheckboxWidget, name "ForceCAS"** |
| `ms_pmwVariableRateShading` | +0x200 | 0x17 | multichoice |

`ren_iRescalingFilter` is not in either cvar list, so a change does not trigger `ms_bGfxRestart`, the ApplyChanges
dialog or KeepSettings. The changed-cvars collector 0x140872710 only matches names in +0x160.

### 1.2 Construction (`gfxopt_init` 0x140872c60, block 0x140873967..0x140873c73; SM block 0x1408738f8..)

```
w = alloc(0x1f0, type 0x142290d98); CMultichoiceWidget_ctor 0x1409179b0(w, menu)   // SM 0x1409180a0
this->ms_pmwRescalingFilter = w; add_child(w, panel)
w->name(+0x08) = "RescalingFilter"; set_label(w, "ETRSGPUMenu.RescalingFilter=Rescaling Filter", 1)
tooltip "ETRSGPUMenu.RescalingFilterTooltip=Rescaling quality when sub-sampling the rendering drawport."
coll = alloc(0x18, type 0x142275e08); {ptr=0, count=0}                              // CWidgetCvarOptionCollection
link = wlink_bind 0x140158a50(w, "ren_iRescalingFilter"); link->coll(+0x10) = coll
for i in 0..5: optcoll_add 0x14015e3c0(coll, &CString(label_i), &CString("i"))
cb = alloc(0x198, type 0x142290c60); CCheckboxWidget_ctor 0x140907480(cb, menu)
this->ms_pcwForceCAS = cb; label "ETRSGPUMenu.RescalingFilter5=FidelityFX CAS"; name "ForceCAS"
wlink_bind(cb, "ren_bForceCAS"); tooltip "ETRSGPUMenu.ForceCASTooltip=Use contrast adaptive sharpening."
```

Layouts:
- **`CWidgetCvarLink`** (0x20 bytes, type 0x142275e10, ctor 0x14015ddb0):
  - +0x00 `CString` cvar name;
  - +0x08 `CString` snapshot, taken by `wlink_snapshot` 0x14015ecd0 at bind, at popup open and at Apply;
  - +0x10 collection pointer (or null);
  - +0x18 optional value-transform function.
  - `wlink_bind` appends the link to the widget's link array: *widget+0x50* pointer, *+0x58* count, *+0x5c* capacity.
- **Collection** (0x18 bytes): +0x00 option array, +0x08 count, +0x0c capacity.
- **Option** (stride 0x30, ctor 0x14015dde0):
  - +0x00 `CString` label;
  - +0x08 pointer (0; returned to the list as the item tag by 0x14015e510);
  - +0x10 `CString` value;
  - +0x18 `CString`;
  - +0x20/+0x28 alias array (added by 0x14015e160, e.g. `"D3D11"` for the API widget).
- **Item count.** The widget has no fixed count. `mc_build_list` 0x140919720 walks every link's collection and emits
  one list item `{label, original label, tag}` per option. The arrows (`0x140918000` returns this count), the popup
  and `mc_setsel` all use this list. **Options added to the collection before the first update appear everywhere.**
- **Labels.**
  - Display goes through the widget's label cache (*+0x190*, count *+0x198*, stride 0x18: raw label, display text,
    width). The cache is rebuilt by 0x1409190c0 / 0x140919370 when *+0x1d8* < 0; the ctor sets −1.0.
  - 0x140919370 (and `is_trans` 0x1410f7ea0 for other text) treats a label as a translation key only if it starts with
    `ETRS`, `DTRS` or `TTRS` **and** contains `=`. Then it shows the text after `=` or its translation. Otherwise the
    label is used verbatim.
  - **So `"DLSS"` and `"FSR 3.1"` need no translation entry.** A key such as `ETRSGPUMenu.RescalingFilterDLSS=DLSS`
    would instead go through the translation lookup.

### 1.3 Selection → cvar, and cvar → selection

`mc_setsel` 0x140918bf0 `(w, idx, noWrite)`:
```
w->+0x178 = idx
if (!noWrite) {
  list = mc_build_list(w); item = list[idx]                               // no bounds check on idx
  for link in w->links: coll = link->+0x10
    for o in coll: if ((label(o)==item.label || label(o)==item.orig) && tag(o)==item.tag)
        wlink_set(link, value(o))                                          // 0x14015ed30
  fire script event "OnChange"
}
```
`wlink_set` 0x14015ed30 `(link, const CString* v)`:
- applies the optional transform (+0x18);
- escapes backslashes;
- formats `"%1=%2"` (name, v);
- runs `lua_exec_string(shell 0x140b75370(), ctx, cmd, 1)`.
- The thunk **0x14015ecc0** = `wlink_set(link, &link->snapshot)` is the revert-all path 0x140875540, used by
  KeepSettings "No" / timeout through the step when *+0x17c* is set.
- The checkbox writes "0"/"1" through the same function (0x140907c00).

`mc_sync` 0x1409199e0 `(w, CString* outLabel, void** outTag) → index`. It is called by widget vt+0x20 0x1409188e0,
which stores the result in *+0x178* and the label on the child button *+0x1b0*, and by the popup open 0x140918dd0.
```
for link in w->links:  txt = wlink_get(link)                               // 0x14015eaf0 = cvar_get_str 0x14112ad30(name)
  i = optcoll_find(link->coll, txt)                                        // 0x14015e5d0
  if (i != -1 && cache has the label) { *outLabel = cache[i].display; return i }
fallback: manual items (+0x180) at +0x178, else label +0x168 "ETRSMenu.Custom=Custom", return -1
```
`optcoll_find` matches, per option in order:
- the exact value text;
- the value with its quotes stripped (only when it starts with `"`);
- its aliases;
- else the nearest numeric value within 1 %.

A non-numeric value that does not start with `"` (e.g. `ss4smvr:dlss`) matches **only** by exact text.

### 1.4 Other paths that touch the link

| path | function | effect on `ren_iRescalingFilter` |
|---|---|---|
| Default / ResetToDefaults_OnYes | 0x140877700 → 0x140135880 → **`wlink_reset` 0x14015eba0** per link | writes the cvar's default text (2nd output of `cvar_get_str`) via Lua directly (**not** through `wlink_set`) |
| KeepSettings No / timeout | step → 0x140875540 → 0x14015ecc0 → `wlink_set(snapshot)` | restores the snapshot value |
| "has changes" / Apply enable | 0x140875190 / 0x140872710 (uses `wlink_get`) | compares only against `ms_astrGfxRestartCvars`: not affected |
| performance presets | `CMSPerformance` 0x1408c6ff7 | reads `ren_fMax3DRenderingMPix` only |

### 1.5 Visibility (`gfxopt_step` 0x1408760d0, vt+0x178, called each step for the top menu by the manager at 0x1400610ef)

```
(w,h) = 0x140876b10()  // gfx_pixRes{Width,Height} (fullscreen) or gfx_pixWin* (windowed)
if (w*h/1e6 <= ren_fMax3DRenderingMPix) { hide(ms_pmwRescalingFilter); show(ms_pcwForceCAS) }
else                                    { show(ms_pmwRescalingFilter); hide(ms_pcwForceCAS) }
```
- show/hide:
  - Sam4: `0x14015ac20` = `flags(+0x10) &= ~0x20`, `0x1401595a0` = `|= 0x20`.
  - SM: `0x14015b8c0` / `0x14015a240`, same bytes.
- The visible test 0x1401595b0 (SM 0x14015a250) requires bit 0x20 clear.
- With the default MPix cap and a resolution under it, **the user never sees the Rescaling Filter row**. This is why
  the post-step un-hide is needed.

## 2. Hook design (recommended)

**Gate:** install all four hooks or none. The gate is:
- the symbols ported;
- a flat upscaler backend available: `up::active() != Off` after `up::init`, i.e. the Vulkan API;
- `deviceEye() == -1` is **not** required, because the menu is shared.

Sentinels are `ss4smvr:dlss` and `ss4smvr:fsr31`. If one ever reached Lua, `ren_iRescalingFilter=ss4smvr:dlss` is a
syntax error ("function arguments expected"), so nothing would be assigned. This is defence in depth only [M].

```cpp
static bool isFilterLink(void* link) {                      // link+0 = CString name
    const char* n = link ? *(const char**)link : nullptr;
    return n && strcmp(n, "ren_iRescalingFilter") == 0;
}
static int g_menuMode;                                       // 0 stock, 1 DLSS, 2 FSR 3.1 (persisted in ss4smvr state file)
static const char* kVal[3] = {nullptr, "ss4smvr:dlss", "ss4smvr:fsr31"};

void hkGfxoptInit(void* self) {                              // gfxopt_init, vt+0x120
    realGfxoptInit(self);
    void* w = *(void**)((uint8_t*)self + 0x1f0);             // ms_pmwRescalingFilter (reflected)
    if (!w) return;
    void** links = *(void***)((uint8_t*)w + 0x50); int n = *(int*)((uint8_t*)w + 0x58);
    for (int i = 0; i < n; ++i) if (isFilterLink(links[i])) {
        void* coll = *(void**)((uint8_t*)links[i] + 0x10);
        if (!coll || *(int*)((uint8_t*)coll + 8) != 6) break;   // expect exactly the 6 stock options
        add(coll, "DLSS", kVal[1]); add(coll, "FSR 3.1", kVal[2]);  // add = cstring_ctor x2, optcoll_add, cstring_dtor x2
        *(float*)((uint8_t*)w + 0x1d8) = -1.0f;              // force label-cache rebuild (ctor value; harmless if already -1)
        break;
    }
}
void hkWlinkSet(void* link, const char** v) {                // wlink_set
    if (isFilterLink(link)) {
        for (int m = 1; m <= 2; ++m) if (*v && strcmp(*v, kVal[m]) == 0) { setMenuMode(m); return; }  // swallow
        setMenuMode(0);                                      // a stock filter (or revert) was chosen
    }
    realWlinkSet(link, v);
}
const char** hkWlinkGet(void* link, const char** out) {      // wlink_get: returns out
    realWlinkGet(link, out);
    if (g_menuMode && isFilterLink(link)) cstring_assign(out, kVal[g_menuMode]);
    return out;
}
void hkWlinkReset(void* link) { if (isFilterLink(link)) setMenuMode(0); realWlinkReset(link); }   // optional
void hkGfxoptStep(void* self) {                              // gfxopt_step, vt+0x178
    realGfxoptStep(self);
    void* w = *(void**)((uint8_t*)self + 0x1f0);
    if (w && upscalerAvailable()) *(uint32_t*)((uint8_t*)w + 0x10) &= ~0x20u;   // keep the row visible
}
```

Notes:
- **Options 6/7 must be appended after the six stock ones.** The indices of 0..5 stay equal to the cvar values;
  nothing in the engine assumes that, but it keeps logs readable.
- **Never write `ren_iRescalingFilter`.**
  - While the mod mode is on, the cvar keeps the user's last stock filter (default 5).
  - That value is what the engine uses whenever the mod upscaler does not run for a frame: phase A, an MSAA > 1 gate,
    or a backend failure.
  - After uninstalling, the game sees a valid 0..5 value.
  - The mod's own choice lives in the mod's file. `se4::options` is read-only at attach, so a small state writer is
    needed.
- **Revert and Default.**
  - KeepSettings-No restores the snapshot through `wlink_set`, so the mod mode is cleared. This matches what happens to
    stock filters.
  - Default goes through `wlink_reset`, so it needs the optional hook to also clear the mod mode.
- **Leave the ForceCAS checkbox to the engine.** With the mod upscaler in phase B the rescale, and with it CAS, is
  skipped for the main view (R25 §3.2, R27 §4), so ForceCAS + DLSS is harmless.
- **Alternative (not recommended):** hook `mc_setsel`/`mc_sync` with a widget-pointer filter. They have 52 and 2
  callers, need bounds-checked list access, and must reproduce the cached label. The link hooks are smaller and
  name-filtered.

## 3. Render-time use and persistence

### 3.1 Consumers of `ren_iRescalingFilter` (0x14224d36c; SM 0x14222c110)

`cvar_readers.py` lists 10 sites. Six are false positives (neighbouring globals 0x14224d368 / 0x14224d370). The real
readers:

| site | function | use |
|---|---|---|
| 0x140bc0f0c (SM 0x140bc15fc) | `execute_view` 0x140bc0d20 tail (R25 §3.2) | rescale mode, below |
| 0x1407bf906 / 0x1407bf916 / 0x1407bf9a0 | scope view 0x1407bf460 (SM 0x1407be9e0, R27 table) | save, write 1, render, restore (in memory) |

Mode selection in `execute_view` (ebx = cvar):
- canvas == eye box → mode 5;
- else if canvas/eye width ratio > 2.0625 (0x141e1d754) → mode 6;
- else if ebx == 5 → mode 4 when either axis ratio > 1 (downscale). Otherwise, when the ratio < [0x141e1d750] and
  global 0x1422d4d58 == 0 → filtered copy; else mode 5;
- else if ebx == 0 → `gfx_copy(…, filtered=0)`;
- else if ebx ≤ 1 (**including negatives**) → clear the borders, then `gfx_copy(…, filtered=1)`;
- else `rescale_pass(canvas, eye, ebx)`.

`rescale_pass` 0x140bbf2c0 (SM 0x140bbf9a0; identical compare chain `cmp ebp,5/4/3` and strings in both):
- 5 → CAS 0x140ec8cb0 (SM 0x140ec9cb0; it uses `gfx_iCASQuality` 0x1422efdc0 clamped 0..3 and `gfx_iCASSharpness`
  clamped 0..10);
- ratio ≥ 2.0625 → "Wide-kernel";
- < 4 → "Bicubic" (B = −1 for 3, −0.25 otherwise);
- 4 → "Lanczos";
- **anything else → "Average"**.

`ren_bForceCAS` (0x1422efdc4; SM 0x1422ccf54) has one real reader: RenderView 0x140bc24b0 (forces the 3D canvas at
equal size, R27 §1.5).

### 3.2 Persistence

| fact | Sam4 | SM | evidence |
|---|---|---|---|
| flags `ren_iRescalingFilter` / `ren_bForceCAS` / `ren_fRenderingPixelRatio` / `ren_fMax3DRenderingMPix` | 0x10006 / 0x10006 / 0x10006 / 0x40006 | same | harvest records (`flags` = record+0x38) |
| bit 4 = SAVED | message "…must be also flagged as SAVED in order to be saved properly into .ini" tests `flags & 4` (0x1401b71f0) | – | string 0x14165b6a0 |
| `.ini` writer: saves only `flags & 4` when called with 1 | 0x1410d7ee0 → test at 0x1410d7f98 | 0x1410d86f0, test at 0x1410d87ae | disasm |
| `prjSaveCVars` → writer(path, 1) | 0x140039f00 → 0x1410dbad0 | 0x140039f80 → 0x1410dc2d0 | harvest name |
| save at app shutdown | 0x14003a4f8 in 0x14003a0c0 | 0x14003a588 | callers |
| project `.ini` path built as `%1%2.ini` | 0x14003bd67 in 0x14003b4a0 | – | string 0x14156a4e8; location per `docs/re/LAB_NOTES.md` |
| file-init values | filter 5, pixel ratio 1.0, MPix and ForceCAS in .bss (set by the `.ini`/presets) | same | .data read |

Runtime cvar objects carry the flags at +0x38, which the writer tests. That they equal the harvested record flags is
inferred (R23/R26 make the same assumption) [M-H]. The out-of-range consequence in §0.3 is therefore real: a value of 6/7
would be saved and reloaded.

## 4. Resolution scale for the upscaler

| setting | Sam4 / SM | menu | range / clamp | use |
|---|---|---|---|---|
| `ren_fMax3DRenderingMPix` | 0x1422ab2f4 / 0x1422884b4 | GPU menu "Max 3D rendering Mpix" (strings at 0x14160dc9c…: "1.0","1.5","2.1","3.0","3.7","5.8","8.3","0") | limiter clamps 0..1000 (R13 §5) | limiter: `s = MPix/(w·h/1e6)`; if `s < 0.95`: both sides × √s |
| `ren_fRenderingPixelRatio` | 0x14224cdd4 / 0x14222bb84 | none (console / `.ini`) | 0.0625..16 (R13 §5) | limiter: both sides × ratio |
| `vr_fRenderingPixelRatio` | 0x14224cb38 / R13 | VR row `ms_hmwRenderingRatioVR` | – | eye RT size (VR only) |

**Recipe for the render size.**
- In the existing `mpix_limiter` hook, for the flat main view (`ret == rv_limiter_call+5 && t_flatMain`, R27 §5.4)
  with `g_menuMode != 0` and phase B:
  1. Call the original first. Take `rw = *outW`, `rh = *outH` (the engine's own size for this output).
  2. Clamp each axis to ≥ ⌈out/3⌉, the DLSS / FSR 3.1 ultra-performance limit.
  3. Use `(rw, rh)` instead of `up::renderSize()`.
- When the limiter left the full size (cap Unlimited or ≥ 95 %), this is native AA (DLAA / FSR native).
- The menu mode chooses the backend (DLSS or FSR 3.1) and overrides `upscaler`/`upscale_quality` from `options.txt` for
  the flat view.
- Reading the two cvar globals directly each frame is also safe (aligned 32-bit floats, written only by Lua on the game
  thread). It would duplicate the limiter maths, though.
- Example at 2560×1440 (3.69 MP) with "2 (FHD 1080p)" = 2.1: √(2.1/3.69) = 0.755, giving 1932×1087.

## 5. Implementation recipe

1. **Symbols** (`manual.json`; `tools/symbols.txt` lists them):
   - new: `gfxopt_init`, `gfxopt_step`, `wlink_get`, `wlink_set`, `wlink_reset`, `optcoll_add`, plus the references
     `wlink_bind`, `optcoll_find`, `mc_setsel`, `mc_sync`;
   - existing: `cstring_ctor`, `cstring_assign`, `cstring_dtor`, `mpix_limiter`, `rv_limiter_call`;
   - vars: `ren_iRescalingFilter`, `ren_fRenderingPixelRatio`, `ren_fMax3DRenderingMPix` (read only).
2. **Checks before installing.** Verify each of these, or skip the menu feature with a log line:
   - `*(void**)self == CMSGraphicsOptions vtable` at the first init call (Sam4 0x141b8dc20, SM 0x141b72470). Log a
     mismatch and stay inert. The vtable itself is not in `manual.json`; compare against the vt+0x120 slot value =
     `gfxopt_init` instead.
   - The widget at +0x1f0 has the multichoice vtable (Sam4 0x141bdc678, SM 0x141bc0f18), or its vt+0x08 returns the
     type record. Optional.
   - Exactly one link named `ren_iRescalingFilter`, with 6 options whose values are "0".."5" (read via +0x10 of each
     0x30 stride).
3. **Hooks:** §2 (`gfxopt_init` post, `wlink_set`, `wlink_get`, `gfxopt_step` post, optional `wlink_reset`).
4. **State:**
   - `g_menuMode` is loaded from and saved to the mod's state file (e.g. `ss4smvr/state.txt`, `flat_upscaler=dlss|fsr31|off`)
     on change.
   - At attach it pre-selects the backend.
   - `setMenuMode` takes effect on the next frame: reset the flat history and recreate the feature if the backend
     changes.
5. **Upscaler:** the R27 flat path. Enable it when `g_menuMode != 0`, using the render size from §4. With
   `g_menuMode == 0` the engine's stock filter runs as usual.

Logging:
- `menu: rescaling options appended (link %p coll %p count %d->%d)`;
- `menu: rescaling select %s (cvar stays %d)`;
- `menu: rescaling stock %s -> mode off`;
- `menu: rescaling default/revert -> mode off`;
- `flat limiter (menu %s): out %dx%d -> engine %dx%d (MPix %.2f ratio %.3f) -> render %dx%d`.

## 6. Runtime checks (a lab run of SS4/SM needs the user's go-ahead first, see the lab notes and the saves snapshot rule)

1. Options > GPU shows 8 entries in the Rescaling Filter list (arrows and popup), with "DLSS" and "FSR 3.1" verbatim.
2. Selecting DLSS:
   - logs the swallowed write;
   - the console value of `ren_iRescalingFilter` is unchanged;
   - the row keeps showing "DLSS" across frames, which confirms vt+0x20 re-syncs each frame through `wlink_get`;
   - no Lua error appears in the log.
3. Selecting Lanczos afterwards writes 4 and turns the mode off. Default writes 5 and turns the mode off.
   KeepSettings-No after a resolution change restores the snapshot and turns the mode off.
4. With a resolution under the MPix cap, the row is visible (post-step un-hide). The ForceCAS checkbox is also visible
   and the layout does not overlap.
5. After quitting, the `.ini` (backed up per LAB_NOTES) contains `ren_iRescalingFilter` 0..5 and no mod value. After the
   mod is removed, the menu shows the stock entry for that value, not "Custom".
6. Changing "Max 3D rendering Mpix" while DLSS is on changes the flat render size in the limiter log
   (`hud_bShowRenderingResolution` also shows it). "Unlimited" gives native AA.
7. On D3D11, or with no backend, the gate leaves the menu stock: 6 entries, no hooks.

## 7. Address table (Sam4 616154 / SamSM 628492)

Rows marked * are in `docs/re/builds/signatures.json` / `manual.json`. They are unique in Sam4, Sam4_Unrestricted, SamSM
and SamSM_Unrestricted: `tools/port_symbols.py` printed no FAIL line for any new name. Its FAIL lines are pre-existing
Unrestricted-only entries; there are none for Sam4 or SamSM.

| name | Sam4 | SamSM | how SM was found / note |
|---|---|---|---|
| `CMSGraphicsOptions` vtable | 0x141b8dc20 | 0x141b72470 | `vtable_slot.py of` the ported init/step |
| `gfxopt_init`* (vt+0x120) | 0x140872c60 | 0x140872bd0 | sig 48; `sig_at` 68/68 of 96 |
| `gfxopt_step`* (vt+0x178) | 0x1408760d0 | 0x140876040 | sig 48; visibility block 0x140876337 / 0x1408762a7 |
| members `ms_pmwRescalingFilter` / `ms_pcwForceCAS` | +0x1f0 / +0x1f8 | same | reflection (`meta_members.py`) |
| CMultichoiceWidget ctor / vtable / type record | 0x1409179b0 / 0x141bdc678 / 0x142290d98 | 0x1409180a0 / 0x141bc0f18 / – | init disasm (SM `lea` at 0x1409180bd) |
| CCheckboxWidget ctor / vtable | 0x140907480 / 0x141bd56f0 | – | decompile |
| `wlink_bind`* | 0x140158a50 | 0x1401596e0 | sig 64; 76/76 |
| `wlink_get`* | 0x14015eaf0 | 0x14015f7a0 | sig 40; 80/80; 4 callers in both |
| `wlink_set`* | 0x14015ed30 | 0x14015f9e0 | sig 48; 76/76; 4 callers in both |
| `wlink_reset`* | 0x14015eba0 | 0x14015f850 | sig 48; 76/76 |
| `wlink_snapshot` / revert thunk | 0x14015ecd0 / 0x14015ecc0 | 0x14015f980 / 0x14015f970 | sigscan (snapshot sig matches 2: by order) / sig 48 |
| `optcoll_add`* / `optcoll_find`* | 0x14015e3c0 / 0x14015e5d0 | 0x14015f060 / 0x14015f270 | sig 48; 96/96 and 88/88 |
| `mc_setsel`* / `mc_sync`* / `mc_build_list` | 0x140918bf0 / 0x1409199e0 / 0x140919720 | 0x1409192f0 / 0x14091a170 / 0x140919eb0 | sig 48 / sig 32 (same function, different registers) / sig 48 |
| list count / popup open / label cache add | 0x140918000 / 0x140918dd0 / 0x140919370 | 0x1409186f0 / – / – | sig 32 |
| widget hide / show / visible test | 0x1401595a0 / 0x14015ac20 / 0x1401595b0 | 0x14015a240 / 0x14015b8c0 / 0x14015a250 | disasm (identical bytes) |
| revert-all links / reset-all to defaults / Default handler | 0x140875540 / 0x140135880 / 0x140877700 | 0x1408754b0 / 0x140136250 / – | sig 48 |
| changed-cvars collector / has-changes | 0x140872710 / 0x140875190 | 0x140872680 / – | sig 32 |
| `cvar_get_str` (name → value, default) | 0x14112ad30 | 0x14112b4e0 | sig 48 |
| `lua_exec_string` (existing) | 0x140b748a0 | 0x140b74e10 | `manual.json` |
| `is_trans` / translate | 0x1410f7ea0 / 0x1410f8e10 | – | decompile |
| `execute_view` filter read / `rescale_pass` / CAS pass | 0x140bc0f0c / 0x140bbf2c0 / 0x140ec8cb0 | 0x140bc15fc / 0x140bbf9a0 / 0x140ec9cb0 | R25; compare chain identical |
| scope view (forces filter 1) | 0x1407bf460 | 0x1407be9e0 | R27 table |
| `prjSaveCVars` / `.ini` writer / SAVED test | 0x140039f00 / 0x1410d7ee0 / 0x1410d7f98 | 0x140039f80 / 0x1410d86f0 / 0x1410d87ae | harvest / disasm |
| cvars `ren_iRescalingFilter` / `ren_bForceCAS` / `ren_fRenderingPixelRatio` / `ren_fMax3DRenderingMPix` / `gfx_iCASQuality` | 0x14224d36c / 0x1422efdc4 / 0x14224cdd4 / 0x1422ab2f4 / 0x1422efdc0 | 0x14222c110 / 0x1422ccf54 / 0x14222bb84 / 0x1422884b4 / 0x1422ccf50 | harvest |

`tools/symbols.txt` gained `gfxopt_init`, `gfxopt_step`, `wlink_bind`, `wlink_get`, `wlink_set`, `wlink_reset`,
`optcoll_add`, `optcoll_find`, `mc_setsel`, `mc_sync` and the vars `ren_iRescalingFilter`, `ren_fRenderingPixelRatio`,
`ren_fMax3DRenderingMPix`. `src/se4/symbols_gen.hpp` was not regenerated.

Confidence:
- **[H]:**
  - the screen class, the members, the widget/link/collection layouts and the construction;
  - option index = cvar value 0..5, with entry 5 = CAS through `ren_iRescalingFilter` (not ForceCAS);
  - `mc_setsel` → `wlink_set` → Lua as the only write path of a selection;
  - the `mc_sync`/`wlink_get`/`optcoll_find` read-back and plain-text labels;
  - the visibility rule;
  - the execute_view / rescale_pass handling of out-of-range values;
  - the SAVED bit test in the writer;
  - the SM ports.
- **[M]:**
  - widget vt+0x20 runs every frame (check 2);
  - the label cache picks up options appended in the init post-hook without a forced rebuild. The recipe forces one
    anyway.
  - the runtime flags equal the harvested record flags;
  - the Lua syntax-error fail-safe of the sentinel;
  - the layout with both rows visible (check 4).
- **Not determined statically:**
  - the user's current `.ini` values (not read);
  - the default MPix cap that the presets apply.
