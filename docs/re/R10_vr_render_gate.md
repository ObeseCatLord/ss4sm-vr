# Gate R10: why the VR view never runs, and how to make the engine take it

Static analysis only (Ghidra projects `sam4`/`fusion` copied to `lab/ghidra_r10`, capstone, no game run).
Sam4/SamSM addresses are PE VAs; Fusion addresses are ELF VAs of `Sam2017_VR.jotn-vr-stock` ("Fusion VR")
or `Sam2017.jotn-vr-stock` ("Fusion flat", the non-VR exe in the same folder). Decompiles: `lab/r10_*.c`.
Per-build addresses come from `tools/render_gate.py` (new, read-only) and `tools/sigscan.py`.

## 1. Answer

**Nothing calls CProjectInstance slot 0x58 (RenderVR) in SS4, and no runtime condition is involved.** The
choice between Render and RenderVR is made at compile time, in the application main loop, by which vtable slot
the per-frame render call uses. SS4 and SM were built from the flat configuration, so the main loop calls
slot 0x50 (Render). No predicate, flag or cvar can redirect it. A VR exe of the same engine branch (Fusion VR)
has the same main loop with the slot changed to RenderVR.

| | main loop | per-frame render call | slot | args |
|---|---|---|---|---|
| Fusion VR | 0xe87f10 | 0xe887cb `call [rax+0x60]` | RenderVR 0xe9c300 | (pi, &dp) |
| Fusion flat | 0xe83ef0 | 0xe8479b `call [rax+0x58]` | Render | (pi, &dp) |
| Sam4 | 0x14003a810 | 0x14003aa6c `call [rax+0x50]` | Render 0x140053b70 | (pi, &dp, 0) |

Fusion flat still contains RenderVR in its CProjectInstance vtable 0x28ef450, at slot 0x60 = 0xe97d10,
whose body is the same as 0xe9c300: it lazily creates the viewer at +0x370 and tail-jumps to the VR view
0xf1d810. Only the slot number at the main-loop call site differs between the two Fusion exes.

`prjIsVRActive`/`CorIsAppVR`/`IsAppVR` are the same kind of build constant, not a runtime predicate. Their
bodies are `mov rax,[first arg]; xor/mov esi,K; jmp [rax+slot]`: Fusion VR 0xe8e620/0x222b130/0x1ad3be0
use K=1 and slot 0x18, Fusion flat 0xe8a030/0x2226b70/0x1ad05c0 use K=0 and slot 0x18, and SS4 0x140062670
uses K=0 and slot 0x10 (MSVC −8). All three script functions, with different signatures (16, 1, 7), share
that one body, and only the first argument is used. So the object is the script call's return-value/argument
frame, and its vt[0x10]/[0x18] stores a BOOL result. Its exact class is **unknown** (confidence M). The main
loop never consults it. Making it return 1 would change only script/content behaviour (R3/R4), not rendering.

## 2. The main loop (Sam4 0x14003a810)

Decompile `lab/r10_s50.c`; disassembly 0x14003a978..0x14003aad5.
- `pi = FUN_140b7c920()->vt[0xa8]()`. 0x140b7c920 returns global 0x1422ab1c8, and pi is stored in
  **0x142273478**, the project instance (CProjectInstance, vtable 0x141582e48).
- Per frame: `pi->vt[0x48]()` at 0x14003a982 (0x1400587a0 → game loop 0x140058800, which calls device 0x50 and
  0x140b6a8e0). If it returns 0 the loop exits.
- `w = 0x140b5b220()` (returns 0x1422aa998, the main window). If `w` is non-null, 0x38 bytes of `*(w+8)`
  (a CDrawPort) are copied to `[rsp+0x60]` (0x14003a9af..0x14003a9e8).
- If the unnamed global **0x1422ab3b0** is 0, then at 0x14003aa62: `rcx=[0x142273478]; rdx=rsp+0x60;
  r8d=0` (from 0x14003a9f5), `call [rax+0x50]` at 0x14003aa6c (bytes `FF 50 50`, return address 0x14003aa6f).
  If 0x1422ab3b0 is not 0, the screen is cleared instead (gfx vt 0x258/0x268) and nothing is rendered.
- Result r ≠ 0 → overlay 0x140b69920(&dp, 0x140b69900(), 0x140b7cc30(pi), 0) and present
  **0x140b5ca90(gfx, 0, 0)**, which contains device slots 0x40/0x48. r = 0 → no present that frame. On the render branch
  0x140b5a610 follows; then 0x140b57640(&dp), which is the drawport dtor.

Fusion VR 0xe87f10 (`lab/r10_f4.c`) has the same structure. The project instance is in 0x397ca80; the step is
vt[0x50]; the skip flag is 0x41a96a0 (↔ 0x1422ab3b0); RenderVR's result gates overlay 0x1f64550 and present
0x1fb3590 (↔ 0x140b5ca90). Neither main loop checks the device global around the call: the Fusion VR exe
always has a device (SteamVR or the emulated fallback).

## 3. RenderVR and its argument

- 0x1400552e0 (SS4 slot 0x58, `lab/set360.c` and disassembly): if `pi+0x360` is null, it allocates the viewer
  (0x1410c52f0(0x138, 0x140039fc0(0)), ctor 0x1401d7d90). Then it **tail-jumps** (0x140055337) to the VR view
  **0x1401dcbe0(viewer, pi, dp)**, so RenderVR returns the VR view's result.
- 0x1401dcbe0 always returns **1** (`mov eax,1` at 0x1401dd7ca; SamSM 0x1401dd402). Main-loop overlay and
  present therefore run every frame, as they do after a successful Render.
- The argument is the same `&dp` the main loop passes to Render: the stack copy of the window drawport. The
  VR view gives it to device slot 0x120, drops it when slot 0x1b8 returns 0, splits its box (+0x10..+0x1c) for
  the desktop mirror (0x140b57de0, 0x1401de680), and draws the HUD/console into it (0x14004c500, 0x140055f10).
- **0x1401dcbe0 dereferences the device global without a null check** (first statement: `[0x1422aad10]->vt[0x30]()`).

### Render's third argument (SS4 only)

`Render(pi, dp, int inVR)`, 0x140053b70, which stores r8d at entry. The main loop passes 0. Each eye inside
the VR view passes 1:
- immersive eyes: 0x1401deaf0 (called by 0x1401dc520) ends `lea r8d,[r9+1]; jmp [rax+0x50]` at 0x1401debd2/
  0x1401debdd, so the flag is 1;
- cinema texture: 0x1401d96a0 → `pi->vt[0x50](pi, tex, 1)` at 0x1401d9891 (decompile
  `lab/sam4_devcallers_decomp.c`).

Render uses the flag at its exit (0x140b5a610 only when the flag is set; `lab/vp.c`). Fusion's Render takes 2
arguments; SS4 added the flag. So a hook on Render can tell the top-level desktop call (flag 0) from the
nested per-eye calls (flag 1).

### Which VR view branch runs (correction to R1)

In 0x1401dcbe0: if `vr_bForceImmersiveMode` (0x142277ad0) == 0, `viewer+0xd0` == 0, `obj = *(*(pi+0xb8)+0x28)`
(0x14004e4d0, 0x14005fd70) is non-null and `obj->vt[0x1a8]()` ≠ 0, then device+0x08 stays 0. That is the
**cinema** path: 0x1401d96a0 renders the game into the texture held at viewer+0x30; 0x1401da9d0 places it on
the `"VRCinema"` entity of the cinema world, unless `vr_bDisableCinemaWorld` (0x142277adc) is set; 0x1401d9e40
draws it for eyes 1 and 2. In every other case device+0x08 = 1 and +0x0c = 1, the **immersive** path:
0x1401dc520(viewer, pi, 0), with stereo eyes as described in EYE_TARGETS.md. R1's table has +0x08/+0x0c the
other way round ("=1 when not immersive"); the decompile above shows they are 1 on the immersive path. The
class of `obj` and the meaning of its vt[0x1a8]/[0x1b0] are **unknown**; vt[0x1b0] drives the fixed-screen
mode at viewer+0xd0, which uses cvars 0x14224cb08..0x14224cb18.

## 4. Per-build addresses (`tools/render_gate.py`, all signatures unique)

| | Sam4 | Sam4_Unrestricted | SamSM | SamSM_Unrestricted |
|---|---|---|---|---|
| main loop (64-byte sig) | 0x14003a810 | 0x14003a880 | 0x14003a8b0 | 0x14003a920 |
| render call `FF 50 50` (= loop+0x25c) | 0x14003aa6c | 0x14003aadc | 0x14003ab0c | 0x14003ab7c |
| return address (= loop+0x25f) | 0x14003aa6f | 0x14003aadf | 0x14003ab0f | 0x14003ab7f |
| project-instance global | 0x142273478 | 0x142273478 | 0x142250518 | 0x14224f518 |
| CProjectInstance vtable | 0x141582e48 | 0x141582e58 | 0x141564c78 | 0x141564c98 |
| Render (slot 0x50, 64-byte sig) | 0x140053b70 | 0x1400564c0 | 0x140053bd0 | 0x140056510 |
| RenderVR (slot 0x58, existing `viewport_render_vr`) | 0x1400552e0 | 0x140057c30 | 0x140055340 | 0x140057c80 |
| VR view (RenderVR tail jmp) | 0x1401dcbe0 | 0x1401df2a0 | 0x1401dc7c0 | 0x1401dee70 |
| per-eye Render dispatcher (0xf0-byte sig) | 0x1401deaf0 | 0x1401e11b0 | 0x1401de720 | 0x1401e0dd0 |

Viewer field +0x360 and the `mov ecx,r13d` after the call are the same in all four. The device global is
0x1422aad10 in both Sam4 builds, 0x142287ed0 in SamSM and 0x142286ed0 in SamSM_Unrestricted (manual.json).

## 5. Recommended fix

**Hook CProjectInstance::Render (slot 0x50) and send the main loop's top-level call to RenderVR.** This
does what Fusion VR does at 0xe887cb. The world is still drawn once per eye and never once more for the
desktop, because the desktop Render call is *replaced*, not added to.

```cpp
// MinHook detour on project_render (Sam4 0x140053b70; sig 0x140053b70 len 64, unique in all four exes)
using RenderFn   = int(__fastcall*)(void* pi, void* dp, int inVR);
using RenderVRFn = int(__fastcall*)(void* pi, void* dp);          // viewport_render_vr (slot 0x58)
int __fastcall hookedRender(void* pi, void* dp, int inVR) {
    if (inVR == 0 && g_installed && *deviceGlobal == &g_obj && g_vrRenderEnabled &&
        _ReturnAddress() == mainLoopRenderRet)                  // main loop + 0x25f (table above)
        return renderVR(pi, dp);                                // = 0x1401dcbe0(viewer, pi, dp), returns 1
    return realRender(pi, dp, inVR);                            // per-eye calls (inVR==1) pass through
}
```

Preconditions and notes:
1. **Device present.** Redirect only while our device is in the global (0x1401dcbe0 calls `[global]->vt[0x30]`
   unguarded) and until `hookedShutdown` clears it. With the global null, keep calling the real Render.
2. **Only the main-loop call.** Check `inVR == 0` and the return address (main loop + 0x25f; a MinHook
   detour sees the original caller's return address). Then nested per-eye Render(…, 1) calls, and any other
   flag-0 caller not found statically, keep the desktop behaviour, and recursion is impossible.
3. **Device slots that then run every frame** (R1 frame sequence): 0x30, 0x1b8, 0x120(dp), 0x80, 0x38,
   0x28(gfx, 1/2/3), 0x18(1/2), 0x20; per eye 0x60 (through 0x140b6aca0), 0x70, 0x78; 0xd0/0x110/0xe0
   (0x1401db670, 0x1401d8430); present still runs 0x40/0x48. Slot 0x1b8 must return 1 to keep the desktop
   mirror and the overlay drawport.
4. **Branch.** To guarantee the immersive stereo path (0x1401dc520 → eye RTs → slot 0x18), set
   `vr_bForceImmersiveMode` (0x142277ad0) = 1. Without it, any frame where `obj->vt[0x1a8]()` ≠ 0 takes the
   cinema path. That path still calls 0x28/0x18 for eyes 1 and 2 (it draws a screen quad), so submission
   works either way.
5. **Stereo mode.** Two-pass needs `vr_iUsingSinglePassStereo` < 1 (0x1422aacec, set by 0x140b6b5b0 from
   device+0xb1c = 0 → `vr_bSinglePrepareRendering`) or device+0xb44 ≠ 0. Otherwise 0x1401dc520 renders once
   into a double-width holder and copies the halves (EYE_TARGETS.md).
6. The viewer is created lazily by RenderVR, as `ensureViewer` in `src/se4/vr_device.cpp` already does for
   the mouse path. The engine path passes tag `0x140039fc0(0)`, and that code passes tag 0.
7. Thread: the main loop runs on the thread that owns the window and gfx. The detour adds no new thread
   interaction.

Alternative (equivalent, not preferred): while the device is installed, write the call's disp8 in memory
(`0x50` → `0x58` at main loop + 0x25e: Sam4 0x14003aa6e, Sam4_U 0x14003aade, SamSM 0x14003ab0e, SamSM_U
0x14003ab7e) under VirtualProtect, and restore it before the device detaches. This is a code patch, it gives
no per-frame opt-out, and it must be reverted on shutdown, so the Render detour is preferred (as in R3).

Not viable: writing a flag or hooking a predicate (none exists, see §1); calling slot 0x58 from the present
hook (that would draw the world on top of the desktop Render, breaking the AGENTS rule against drawing twice
per frame).

## Unknowns
- Class of the script return frame used by `prjIsVRActive` (§1).
- Class of `*(*(pi+0xb8)+0x28)` and the meaning of its vt[0x1a8]/vt[0x1b0] (cinema / fixed-screen choice).
- Name of the main-loop skip-render global 0x1422ab3b0 (Fusion 0x41a96a0); it has no harvest record.
- Runtime confirmation: the first lab run with the Render detour must show device slots 0x30/0x28/0x18 being
  called.
