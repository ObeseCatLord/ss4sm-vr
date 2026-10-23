# Gate R1 — VR device seam

Status: **outcome B** (consumers intact, factory stripped). Evidence below is for Sam4.exe build 616154
(sha256 fb7aed12…); SamSM addresses to be harvested the same way.

## Device global
- `vrGetDeviceName` (0x140b6a560), `vrGetDeviceAPI` (0x140b6a5c0), `vrIsEyeTracking` (0x140b6a7a0) and
  `vrGetEyeTrackingData` (0x140b6a7f0) all load `qword [0x1422aad10]` and return a default when it is null.
- `tools/riprefs.py`: 345 RIP-relative references to 0x1422aad10 in `.text`; every one is a load or `cmp`
  except `mov qword [0x1422aad10], 0` at 0x140b6af43 (shutdown clear). **No code ever stores a device**,
  so the factory (Fusion's `steamStartVRDevice` / `oculusStartVRDevice` / `emuStartVRDevice`) is gone.
- Plan: after engine init, store a pointer to our `COpenXRDevice` object at 0x1422aad10.

## IsAppVR
`prjIsVRActive`, `CorIsAppVR` and `IsAppVR` share one body at 0x140062670: `mov rax,[rcx]; xor edx,edx;
jmp [rax+0x10]` — the script returns constant false. C++ callers of the underlying predicate may be
constant-folded; find them per feature (Gate R3).

## Vtable layout vs Fusion 2017 VR (Linux ELF, non-PIE)
Fusion `CVRDevice` vtable 0x3024950 (63 slots), `CSteamVRDevice` 0x31325b0, `CEmulatedVRDevice`
0x3131ff0, `CVRInputDevice` 0x312d2d0 (26 slots) — found via typeinfo strings (`tools/elf_vtables.py`).
Fusion device global: 0x41f2d50.

| meaning (from script wrapper)   | Fusion offset | SS4 offset |
|---------------------------------|---------------|------------|
| GetDeviceName(CString*)         | 0x118         | 0x110      |
| GetDeviceAPI() → 1/4/8          | 0x150         | 0x148      |
| GetTrackedDevicesCount()        | 0x188         | 0x180      |
| ShowSteamVRActions              | 0x1f8 (last)  | 0x1f0      |
| IsEyeTracking()                 | —             | 0x1f8      |
| GetEyeTrackingData(...)         | —             | 0x200      |

Constant shift of −8: Itanium emits two destructor slots (D1, D0), MSVC one (scalar deleting dtor). SS4
appends eye-tracking slots after Fusion's last slot. Remaining slot semantics come from Fusion's
`CSteamVRDevice` bodies (OpenVR calls and strings), cross-checked against SS4 call sites
(`tools/device_slots.py`, heuristic: slots above ~0x218 in its output are other objects' calls).
