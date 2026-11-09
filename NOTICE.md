# Notices

SS4SM-VR, a VR mod for Serious Sam 4 and Serious Sam: Siberian Mayhem.
Copyright (C) 2026 ObeseCatLord and contributors.

This program is free software: you can redistribute it and/or modify it under the terms of the GNU General Public
License as published by the Free Software Foundation, version 3 ([LICENSE](LICENSE)). It is distributed in the hope
that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR
A PARTICULAR PURPOSE. See the GNU General Public License for more details.

## Additional permission under GNU GPL version 3 section 7

If you modify this Program, or any covered work, by linking or combining it with the NVIDIA DLSS SDK / NGX SDK (or a
modified version of that SDK), containing parts covered by the terms of the NVIDIA RTX SDKs license, the licensors of
this Program grant you additional permission to convey the resulting work. Corresponding Source for a non-source form
of such a combination shall not include the source code for the parts of the NVIDIA DLSS / NGX SDK used, and the NVIDIA
DLSS runtime (`nvngx_dlss.dll`) may be conveyed alongside it under NVIDIA's own terms.

The same permission is granted for the AMD FidelityFX SDK, the Khronos OpenXR loader and MinHook, whose licenses
(MIT, Apache-2.0, BSD-2-Clause) are GPL-compatible in any case.

## What this repository does and does not contain

- No game files, extracted assets, scripts or decompiled code from Serious Sam 4, Siberian Mayhem or Serious Sam
  Fusion 2017. The `docs/re` notes record addresses, offsets and behaviour found by reverse engineering, for
  interoperability. You need your own copy of the games (Steam builds 616154 and 628492).
- No third-party SDK sources. The build fetches them at pinned versions (see below).
- Serious Sam is a trademark of Croteam / Devolver Digital. This project is not affiliated with or endorsed by
  Croteam, Devolver Digital, NVIDIA, AMD or Valve.

## Third-party components

| component | version | license | use |
|---|---|---|---|
| [OpenXR-SDK](https://github.com/KhronosGroup/OpenXR-SDK) loader | 75c53b6 (1.1.53) | Apache-2.0 | built from source, shipped as `libopenxr_loader.dll` |
| [MinHook](https://github.com/TsudaKageyu/minhook) | c3fcafd (1.3.4) | BSD-2-Clause | linked statically |
| [Vulkan-Headers](https://github.com/KhronosGroup/Vulkan-Headers) | 1.4.363 | Apache-2.0 / MIT | headers |
| [NVIDIA DLSS SDK](https://github.com/NVIDIA/DLSS) | v310.9.16 | NVIDIA RTX SDKs license | headers at build time; `nvngx_dlss.dll` shipped in releases ([licenses/NVIDIA_RTX_SDK_LICENSE.txt](licenses/NVIDIA_RTX_SDK_LICENSE.txt)) |
| [AMD FidelityFX SDK](https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK) | v1.1.4 | MIT | headers; `amd_fidelityfx_vk.dll` (FSR 3.1) shipped in releases |
| SS2 VR mod `src/common` | — | MIT / GPL-3.0 (same author) | maths, comfort, controls and other helpers in `src/core` |

"This software contains source code provided by NVIDIA Corporation." (the DLSS SDK's Vulkan helper header, compiled
into `ss4smvr.dll`).
