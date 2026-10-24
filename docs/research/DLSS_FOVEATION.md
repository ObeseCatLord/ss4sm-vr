# Research: upscaling (DLSS/FSR/XeSS) and foveated rendering for the SS4/SM OpenXR mod

Date: 2026-10-23. Targets: Serious Sam 4 `Sam4.exe` build 616154 (app 257420) and Siberian Mayhem `SamSM.exe`
build 628492 (app 1792250), Serious Engine 4 Vulkan backend, OpenXR via Monado/SteamVR (Proton: wineopenxr).
Local machine facts come from read-only inspection (`strings`, `vulkaninfo`, `ls`). Nothing was modified or run.

Confidence tags: **[verified]** read from source code, binary, SDK header or official doc; **[reported]** press,
forum or community source; **[inference]** our reasoning, not yet tested.

---

## A. Upscaling / DLSS

### A1. Native support in SS4 / SM

**Neither game ships DLSS, DLAA, FSR 1/2/3, XeSS, or a dynamic-resolution controller.**

- **Install directories** [verified]: `Serious Sam 4/Bin/x64` and `Siberian Mayhem/Bin/x64` contain no `nvngx_dlss.dll`,
  `sl.*.dll`, `amd_fidelityfx*`, `ffx*` or `libxess*`. Vendor DLLs present: `amd_ags_x64.dll`, `Tobii.GameIntegration.dll`,
  `Compressonator_MT_DLL.dll`, `dxcompiler.dll`/`dxil.dll`, plus our `ss4smvr/`.
- **Exe strings** [verified]: `strings -n 6 | grep -i` finds no `DLSS|NGX|nvngx|XeSS|Streamline|DLAA|FSR2/3` in
  `Sam4.exe`, `SamSM.exe` or `SamSM_Unrestricted.exe`. The only AMD FidelityFX item is **CAS** (a sharpener).
  The `gfx_*/ren_*/vr_*/sta_*/vk_*` cvar name sets are identical in SS4 and SM (the diff is empty).
- **What the engine does have** [verified, cvar names from the exes]:
  - Drawport sub-sampling plus a spatial rescale pass (`Engine/Rendering/RescaleShader.sh`, `RescalePS`/`BigRescalePS`):
    - `ren_iRescalingFilter`: 0 point, 1 bilinear, 2 bicubic, 3 sharpen-bicubic, 4 Lanczos, 5 FidelityFX CAS.
      Menu text: "Rescaling quality when sub-sampling the rendering drawport".
    - `ren_fMax3DRenderingMPix` (menu: "Maximum allowed 3D rendering resolution measured in milions of pixels").
    - `ren_bForceCAS`, `gfx_iCASQuality`, `gfx_iCASSharpness`, `gfx_bOptimizeCAS`.
    - `vr_fRenderingPixelRatio` (the leftover VR render scale).
  - AA: `gfx_iFXAA`, `gfx_iAntiAliasingSamples` / `gfx_ctMultiSamplesSet` (MSAA). PCGamingWiki lists "FXAA, MSAA up
    to 8X and SSAA up to 4X" and `upscaling = unknown`. **There is no TAA**, so the engine has no jitter pipeline.
    Sources: [PCGW SS4](https://www.pcgamingwiki.com/wiki/Serious_Sam_4), [PCGW SM](https://www.pcgamingwiki.com/wiki/Serious_Sam:_Siberian_Mayhem).
  - Engine VRS (see B6): `ren_bSceneVRS`, `ren_bDepthVRS`, `ren_bTranslucentVRS`, `prt_bEnableVRS`, and the
    menu entries "Variable Rate Shading: None/Particles/Scene/Full".
  - **Motion-vector plumbing exists but is probably dormant.** The shader source embedded in the exe
    (`Sources/Engine/Graphics/Shader/DepthShadowShader.sh`, `GrassShader`, `VegetationVS`) has a
    `GENERATE_MVECS` permutation. The VS outputs `vClipSpacePos` / `vPreviousClipSpacePos` using
    `c_mPrevAbsToClipLeft` / `c_mPreviousModelToClipLeft`. The depth-prepass PS writes
    `vDepth = float4(depth, velocity.xy * c_vViewportSizeInPixels * 0.5, 1)`, which is pixel-space velocity in the
    G/B channels of the depth target. Only *Left* previous matrices are referenced; a `vOutPosPreviousClipRight`
    variable exists but its use is unclear. **RE item:** find what enables `GENERATE_MVECS=1` at runtime (it appears
    in the GrassShader define list) and whether every opaque shader family supports it. It is the best route to
    object-correct motion vectors.
  - Motion blur (`efx_bMotionBlur`, `ppp_fMotionBlur`, `MotionBlurBlendPS`) is frame-blend accumulation. It does not
    use a velocity buffer.
- **Patch notes** [verified, Steam news API for
  [257420](https://api.steampowered.com/ISteamNews/GetNewsForApp/v2/?appid=257420&count=200&maxlength=0) and
  [1792250](https://api.steampowered.com/ISteamNews/GetNewsForApp/v2/?appid=1792250&count=200&maxlength=0)]:
  - No Croteam announcement mentions DLSS, FSR, XeSS, upscaling or VRS.
  - The only graphics-related match is SM Update 1.02, which fixed Vulkan lock-ups and crashes during loading
    ([announcement](https://steamstore-a.akamaihd.net/news/externalpost/steam_community_announcements/6154735015632626832)).
  - The DLSS hits on 257420 are Rock Paper Shotgun "RTX/DLSS games" round-ups.
- **History** [reported]: "Serious Sam 4: Planet Badass" was on NVIDIA's 2018 list of announced DLSS titles
  ([PCWorld](https://www.pcworld.com/article/3299557/components-graphics/nvidia-geforce-rtx-every-pc-game-real-time-ray-tracing-dlss.html)).
  It never shipped. Players were still asking for FSR/DLSS mods in 2023
  ([Steam discussion](https://steamcommunity.com/app/257420/discussions/0/2943620809069701458)).
  The NVIDIA RTX games page did not render a Serious Sam row when fetched.
- **NVAPI**: the exes call `nvapi_QueryInterface` (log string "Failed to initialize NvAPI"), which the D3D11 stereo
  paths use. dxvk-nvapi is now present in the Proton prefix (A3), so the engine may take NV-specific branches
  under Proton that it didn't before. Check `Sam4.log` on the first run.

### A2. Community upscaler mods

- **No SS4/SM-specific DLSS/FSR/XeSS mod was found** (PureDark, OptiScaler, DLSS-Enabler, Nexus). Searches turned up
  only a ReShade preset ([Nexus](https://www.nexusmods.com/serioussam4/mods/3)) [reported].
- **OptiScaler** ([GitHub](https://github.com/optiscaler/OptiScaler)) [verified README]:
  - It is middleware that "intercepts upscaler calls from the game".
  - It works only in games that already integrate DLSS2+/FSR2+/XeSS: "replace upscalers in games that already
    support DLSS2+ / FSR2+ / XeSS".
  - It supports D3D11, D3D12 and Vulkan (on Vulkan: FSR 2.x/3.1/4.x, DLSS, XeSS 2.x). It is loaded as a proxy DLL
    (dxgi/winmm/version-style; details in its wiki) and spoofs nvngx/NVAPI for non-NVIDIA GPUs.
  - **It cannot help here:** SS4 has no upscaler API calls to intercept.
- **PureDark** builds engine-specific mods that create the upscaler inputs themselves. For example, the SkyrimVR
  upscaler is an SKSE plugin with "fixed foveated DLSS" and VRS-based FFR on NVIDIA
  ([Nexus SkyrimVR upscaler](https://www.nexusmods.com/skyrimspecialedition/mods/82179),
  [DSOGaming](https://www.dsogaming.com/mods/skyrim-dlss2-fsr2-xess-upscaler-mod-is-now-available-for-download/)).
  Early VR builds had "very few performance gains" because the whole VR texture was processed, including invisible
  pixels. His RE-engine VR work notes that a DLSS fix still leaves a volumetrics mismatch between eyes "like the old
  AFR method" ([REFramework fork notes](https://newreleases.io/project/github/PureDark/REFramework/release/RE9_AFW_v1.0-beta.6))
  [reported]. Takeaway: a game without native DLSS needs an engine-integrated mod. A proxy DLL is not enough.
- **UEVR** (Praydog) ([GitHub](https://github.com/praydog/UEVR)) [verified README]:
  - It relies on Unreal's own upscaler plugins when "Native Stereo" works.
  - Its fallback "Synchronized Sequential" mode "adds ghosting to some effects like TAA and motion blur". AFR has
    the same problem. Temporal upscalers need per-view history and per-view motion vectors.

### A3. What a VR mod must provide for DLSS SR / DLAA (and how it runs under Proton)

**Inputs** [verified, [DLSS SDK](https://github.com/NVIDIA/DLSS) v310.9.16 released 2026-10-16,
`include/nvsdk_ngx_helpers_vk.h` `NVSDK_NGX_VK_DLSS_Eval_Params`, and `doc/DLSS_Programming_Guide_Release.pdf`]:

| Input | Requirement | SS4/SM status |
|---|---|---|
| Color | Render-resolution, jittered, ideally HDR pre-tonemap ("during the post-processing phase ... before" tonemap/post FX) | Engine renders HDR; the insertion point needs RE (B-pass before bloom/tonemap). |
| Depth | Per-view depth (`pInDepth`) | Available (depth prepass). |
| Motion vectors | "accurate and calculated at 16 or 32 bits per-pixel", per view, pixel space or scaled (`InMVScaleX/Y`), from **unjittered** matrices | Engine `GENERATE_MVECS` path is left-eye-oriented and probably off. Otherwise synthesize camera MVs from depth (see below). |
| Jitter | `InJitterOffsetX/Y` in render-pixel space; Halton with ≥16 phases (32+ preferred); total phases = 8 × (target/render)² | **Mod must add it**: offset each eye's projection (we already build projections from `xrLocateViews`). |
| Exposure | `pInExposureTexture` or auto-exposure flag; `InPreExposure` | Engine has auto-exposure; use the DLSS auto-exposure flag first. |
| Reset | `InReset=1` on cuts, level loads, teleports, menu ↔ game | Hook level load and cutscene state. |
| Mip bias | "negatively bias the LOD for textures" | Hook `vkCreateSampler` and add `log2(render/display)` to `mipLodBias`. |
| HUD/reticle | Not an input. Keep it out of DLSS input | Our HUD/crosshair go to composition layers or are drawn after upscale. Good fit. |
| Optional masks | `pInBiasCurrentColorMask` (reactive), particle mask | Particles/translucents (`ren_bTranslucentVRS` pass) are a candidate for a bias mask. |

**Per-eye contexts** [verified, DLSS guide §3.17 "Multi-view and Virtual Reality Support"]:
- "DLSS natively supports multiple views, including virtual reality (VR) by creating multiple DLSS instances with one
  associated to each view".
- For a shared (double-wide) render target, use subrects (`InColorSubrectBase`, `InDepthSubrectBase`,
  `InMVSubrectBase`, `InOutputSubrectBase`), with `InEnableOutputSubrects=true` at creation.
- **Implementation:** create two `NVSDK_NGX_Handle`s. Evaluate each with its eye's subrect (double-wide path) or its
  own 2D view of array layer *i* (multiview path).

**Motion vectors in VR** [inference]:
- Each eye's MV = `curClip(eye) − prevClip(eye)` using that eye's *previous* head pose. Do **not** reuse the
  left-eye previous matrix for the right eye. The engine's `GENERATE_MVECS` code does exactly that, so it must be
  patched: the right-eye path has to use a right `c_mPrevAbsToClip`.
- Fallback when object MVs are unavailable: a compute pass reconstructs world position from depth and reprojects
  with the previous per-eye view-proj. This is correct for static geometry and camera motion. It ghosts on moving
  enemies, which matters in SS4 with its hordes.
- Keep jitter out of the MV matrices.
- Jitter per eye: use the same Halton index for both eyes (each eye has its own DLSS history). Per-eye phase offsets
  are an optional experiment.

**Vulkan device/instance requirements** [verified, `nvsdk_ngx_vk.h`]:
- Call `NVSDK_NGX_VULKAN_GetFeatureInstanceExtensionRequirements` / `...DeviceExtensionRequirements` *before*
  creating the instance/device.
- Our mod creates both through `xrCreateVulkanInstanceKHR` / `xrCreateVulkanDeviceKHR`, so we can append them.
- The NGX Vulkan path relies on NVIDIA extensions such as `VK_NVX_binary_import` and `VK_NVX_image_view_handle`,
  plus `VK_KHR_push_descriptor` / buffer device address. All are present on this RTX 4090 with driver 615.78
  [verified `vulkaninfo`].

**NGX on Linux** [verified]:
- The SDK ships native `lib/Linux_x86_64/{rel,dev}/libnvidia-ngx-dlss.so.310.9.16` (+dlssd, dlssg) and
  `libnvsdk_ngx.a` (repo tree).
- The host driver ships `/usr/lib/libnvidia-ngx.so.615.78.08` and the Wine shims `/usr/lib/nvidia/wine/{nvngx.dll,_nvngx.dll,nvngx_dlssg.dll}`.
- NGX for Linux arrived with driver 450.51 ([Phoronix](https://www.phoronix.com/news/NVIDIA-450.51-Linux-Driver)).
- Our payload is a Windows DLL inside a Wine process, so it must use the **Windows** NGX path. The native `.so`
  path would only matter for a Linux-native build.
- Known Linux pitfall: NGX tries to write OTA models under `/usr/share/nvidia`
  ([NVIDIA forum](https://forums.developer.nvidia.com/t/dlss-ngx-ignores-config-on-linux-and-requires-write-access-to-usr-share/356828))
  [reported]. Ship `nvngx_dlss.dll` next to our DLL and disable OTA updates.

**DLSS under Proton** [verified, Proton `proton` script on the
[bleeding-edge branch](https://github.com/ValveSoftware/Proton/blob/bleeding-edge/proton), and
[dxvk-nvapi README](https://github.com/jp7677/dxvk-nvapi)]:
- **NVAPI defaults:** current Proton enables dxvk-nvapi **by default**. The old `PROTON_ENABLE_NVAPI=1` (2021,
  [GamingOnLinux](https://www.gamingonlinux.com/2021/10/steam-play-proton-63-7-is-out-now-proton-experimental-gets-dlss-for-dx12-games-on-linux/))
  has been replaced by `PROTON_DISABLE_NVAPI` / `PROTON_FORCE_NVAPI`.
- **NGX DLLs:** Proton copies `_nvngx.dll` and `nvngx.dll` from the driver's `nvidia/wine` directory into
  `system32`. In the SS4 prefix (`compatdata/257420/pfx/drive_c/windows/system32`), `nvapi64.dll`, `nvngx.dll` and
  `_nvngx.dll` are already present [verified `ls`].
- **SS4 Proton quirk:** the only one Proton applies to 257420 is `hidevggpu` (Steam Deck GPU hide). It does not
  hide the NVIDIA GPU.
- dxvk-nvapi "does not implement DLSS ... It mostly forwards the relevant calls". DLSS needs Turing or newer on
  the proprietary driver.
- Vulkan-API DLSS titles (Doom Eternal, No Man's Sky, Wolfenstein Youngblood) were the first DLSS games working
  under Proton in 2021 ([mundobytes summary](https://mundobytes.com/valve-dlss-y-nvk/)) [reported]. Our path
  (Windows NGX over winevulkan) is therefore established.
- Debug env: `DXVK_NVAPI_LOG_LEVEL=info`; DLSS indicator `__NGX_SHOW_INDICATOR=1` on Linux (guide); the Windows
  registry `ShowDlssIndicator` also works in the prefix.

**Vendor-neutral alternatives (Vulkan)**:
- **FSR 3.1**: AMD FidelityFX SDK 1.1 added FSR 3.1 with a Vulkan backend
  ([GamingOnLinux](https://www.gamingonlinux.com/2024/07/amd-releases-fidelityfx-sdk-110-with-fsr-31-and-vulkan-support/),
  [Vulkan backend docs](https://gpuopen.com/manuals/fidelityfx_sdk/fidelityfx_sdk-group__vkbackend)).
  - Inputs match DLSS (color, depth, MVs, jitter, exposure, reactive mask).
  - Use one context per eye. The backend's `maxContexts` counts every context.
  - The prebuilt signed DLL works on any vendor and under Proton without NVAPI.
  - FSR 4 is ML-based and RDNA4-oriented. OptiScaler lists FSR 4.x on Vulkan; check SDK support before depending on it.
- **XeSS 2.x**: Vulkan 1.1 support arrived in XeSS 2.0.1. It runs on non-Intel GPUs with `shaderIntegerDotProduct`,
  `shaderStorageReadWithoutFormat` and `mutableDescriptorType`
  ([release notes](https://newreleases.io/project/github/intel/xess/release/v2.0.1); [repo](https://github.com/intel/xess)).
- **Recommendation**: build one "temporal upscaler" interface fed by the same jitter/MV/depth plumbing.
  - Implement FSR 3.1 first: vendor-neutral, debuggable, no NGX project ID or OTA quirks.
  - Then add DLSS/DLAA behind the same interface.
  - **DLAA (native-res DLSS)** is valuable even without upscaling, because the engine has no TAA and VR shimmer is
  the main image-quality problem.

---

## B. Foveated rendering

### B4. Steam Frame and SteamVR

**Eye tracking** [verified, [Steamworks: Steam Frame, custom engines](https://partner.steamgames.com/doc/steamhardware/steamframe/engines/custom)]:
- Docs: "Our current support for eye tracking uses the XR_EXT_eye_gaze_interaction OpenXR extension."
- Unity and Godot pages say the same
  ([Unity](https://partner.steamgames.com/doc/steamhardware/steamframe/engines/unity),
  [Godot](https://partner.steamgames.com/doc/steamhardware/steamframe/engines/godot)).
- One combined gaze ray; check `XrSystemEyeGazeInteractionPropertiesEXT.supportsEyeGazeInteraction` at runtime.

**OpenXR extensions by runtime flavour** [verified, Khronos
[OpenXR-Inventory](https://github.com/KhronosGroup/OpenXR-Inventory/tree/main/runtimes), SteamVR 2.19.0, updated 2026-10-08]:

| Runtime entry | View configs | Foveation-relevant extensions |
|---|---|---|
| `valve_steamvr_frame.json` (the **on-headset** standalone runtime, Linux/ARM) | PRIMARY_STEREO only | `XR_EXT_eye_gaze_interaction`, `XR_FB_foveation`, `XR_FB_foveation_configuration`, `XR_FB_foveation_vulkan`, `XR_FB_swapchain_update_state`, `XR_META_foveation_eye_tracked`, `XR_META_vulkan_swapchain_create_info` |
| `valve_steamvr_windows.json` / `valve_steamvr_linux64.json` (**PC** SteamVR, which serves a streamed Frame) | PRIMARY_STEREO only | `XR_EXT_eye_gaze_interaction` only. **No** FB/META foveation, **no** `XR_VARJO_quad_views` / `XR_VARJO_foveated_rendering` |

- The Steamworks page lists the FB/META foveation set as "recommended" extensions. Per the inventory, those are
  present on the standalone runtime.
- On the standalone side, `XR_FB_foveation_vulkan` hands the app a runtime-generated fragment-density-map image per
  swapchain image (`VK_EXT_fragment_density_map`). Valve implemented FDM-based foveation in Mesa Turnip for the
  Frame's Adreno GPU ([Phoronix, XDC 2026](https://www.phoronix.com/news/Steam-Frame-Turnip-Foveated)).
- **This RTX 4090 / driver 615.78 does not expose `VK_EXT_fragment_density_map`** [verified `vulkaninfo`]. On a PC
  the FB path is irrelevant even if SteamVR exposed it.
- A community list on the Steam forums contradicts itself on these entries
  ([thread](https://steamcommunity.com/app/250820/discussions/8/3121550424355682585/)). Trust the inventory, and
  confirm with our own `vk_xr_probe` on SteamVR.

**Foveated streaming** [reported]:
- It is done by SteamVR/Steam Link at the encoder. Valve's Jeremy Selan describes always sending two full-FOV views
  plus two zoomed, gaze-centred high-detail regions with more bits
  ([PC Gamer hands-on](https://pcgamer.com/hardware/vr-hardware/hands-on-steam-frame-impressions)).
- It "applies to all streamed Steam content without needing a specific implementation by developers". For games
  with foveated rendering, "the foveated streaming tech works just as well on top of it"
  ([Road to VR](https://roadtovr.com/steam-frame-hands-on-valve-vr-headset-index-2)).
- Foveated streaming reduces bandwidth, not GPU work; foveated rendering reduces GPU work
  ([PCGH](https://www.pcgameshardware.de/Steam-Frame-Hardware-285941/News/PC-VR-Streaming-Foveated-Rendering-1554414/)).
- **The application needs to do nothing for it.** Example of an app doing its own eye-tracked FR on the Frame:
  SUPERHOT VR's standalone build ([MIXED](https://mixed-news.com/en/superhot-vr-eye-tracked-foveated-rendering-steam-frame-default/)).

**Vulkan helpers on PC SteamVR**: none at the OpenXR level (no `XR_FB_foveation_vulkan`, no quad views). A PC Vulkan
app does its own foveation, typically with `VK_KHR_fragment_shading_rate` (or `VK_NV_shading_rate_image`), driven
by `XR_EXT_eye_gaze_interaction`.

### B5. Monado and wineopenxr

**Monado** [verified, source `main` @ fad2c495 (2026-10-23), `extension_support/oxr_extension_support.py`,
`oxr_system.c`, `oxr_session.c`, `comp_compositor.c`, `comp_render_cs.c`]:
- **Supported:** `XR_EXT_eye_gaze_interaction` (CMake `XRT_FEATURE_OPENXR_INTERACTION_EXT_EYE_GAZE`, default ON) and
  `XR_VARJO_quad_views` (default ON), plus `XR_KHR_visibility_mask`, `XR_KHR_composition_layer_depth`, and
  `XR_KHR_vulkan_enable2`.
- **Not supported:** `XR_VARJO_foveated_rendering`, `XR_FB_foveation*`, `XR_META_foveation_eye_tracked`, and
  `XR_HTC_foveation`.
- **Quad-view emulation** (MR "Add support for quad views", merged 2026-06-26,
  [!2799](https://gitlab.freedesktop.org/monado/monado/-/merge_requests/2799)): on a stereo HMD, the main compute
  compositor sets `supports_emulated_quad_views_with_inset`. The state tracker then:
  - exposes `XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO_WITH_FOVEATED_INSET` (OpenXR 1.1) / `PRIMARY_QUAD_VARJO`;
  - returns inset views 2–3 with the context pose and **half the FOV angles** (fixed, centred);
  - splits the 4-view projection layer into a context layer and an inset layer (`XRT_LAYER_SPLIT_QUAD_VIEW_CONTEXT`
    / `_INSET`);
  - **blends the inset edges in the runtime compositor**.
- **Version:** the strings "System has quad views but it is not allowed to expose them" exist in the current Envision
  build (`prefixes/796a3406…/lib/libopenxr_monado.so`, built 2026-10-23) and in `/usr/lib/libopenxr_monado.so`
  (2026-09-23). They are absent from builds from early 2026 [verified `strings`].
- **Gaze-steered inset:** Monado reports `fovMutable = true` (`comp_compositor.c` `supports_fov_mutable = true`). The
  spec allows the inset `fov` to change at runtime as long as its pose equals the stereo view's pose. The app can
  therefore move the inset with gaze itself, even without `XR_VARJO_foveated_rendering` [inference from spec +
  source; test it].
- Our R5 probe already saw `eye_gaze_interaction` through wineopenxr on Monado (`docs/re/R5_graphics_interop.md`).

**wineopenxr (Proton)** [verified, [`wineopenxr/make_openxr`](https://github.com/ValveSoftware/Proton/blob/bleeding-edge/wineopenxr/make_openxr),
`openxr.c`, `openxr_thunks.c`, bleeding-edge, xr.xml 1.1.65; proton_11.0 uses xr.xml 1.1.58]:
- **Blocklist, not an allowlist:** every registry extension is thunked except `UNSUPPORTED_EXTENSIONS` =
  `XR_EXT_debug_utils`, `XR_KHR_loader_init`, `XR_MSFT_perception_anchor_interop`, `XR_HTC_foveation`, and (bleeding-edge
  only) `XR_META_environment_depth`, `XR_META_environment_raycast`, `XR_FB_triangle_mesh`,
  `XR_META_spatial_entity_discovery`, and `XR_BD_camera_image`.
- **Enumeration:** `xrEnumerateInstanceExtensionProperties` returns the **host runtime's list** unchanged, plus
  substitutions: D3D11/D3D12_enable mapped onto Vulkan, and win32 perf-counter mapped onto timespec.
- **Relevant extensions present in the generated supported table:** `XR_EXT_eye_gaze_interaction`, `XR_VARJO_quad_views`,
  `XR_VARJO_foveated_rendering`, `XR_FB_foveation`, `XR_FB_foveation_configuration`, `XR_FB_foveation_vulkan`,
  `XR_FB_swapchain_update_state(_vulkan)`, `XR_META_foveation_eye_tracked`, `XR_META_vulkan_swapchain_create_info`,
  `XR_KHR_visibility_mask`, `XR_KHR_composition_layer_depth`, and `XR_KHR_vulkan_enable2`.
  `XrSwapchainImageFoveationVulkanFB`, `XrFoveatedViewConfigurationViewVARJO` and similar structs have thunks.
- **Net:** whatever Monado/SteamVR expose passes through. View-configuration enums (quad) are plain values. FB
  foveation `VkImage` handles are passed as host handles; this path is untested and irrelevant on our PC GPUs.

### B6. Implementation recommendation for this Vulkan engine

**What the engine already does** [verified strings]:
- It uses `VK_KHR_fragment_shading_rate` only through **per-draw/pipeline rates**: `vkCmdSetFragmentShadingRateKHR`
  plus `vkGetPhysicalDeviceFragmentShadingRatesKHR`, gated by `ren_bSceneVRS`, `ren_bDepthVRS`,
  `ren_bTranslucentVRS` and `prt_bEnableVRS`.
- That is uniform coarse shading, **not foveation**. There is no shading-rate attachment.

**GPU capability** (RTX 4090, 615.78) [verified]:
- `attachmentFragmentShadingRate` texel size 16×16; max fragment size 4×4 (aspect ratio 2).
- `layeredShadingRateAttachments = true`, which allows one array attachment for multiview.
- `fragmentShadingRateNonTrivialCombinerOps = true`.
- `VK_NV_shading_rate_image` is also present. FDM is not.

**The device seam already has an eye-tracking slot**: `vrIsEyeTracking` (vtable 0x1f8) and `vrGetEyeTrackingData`
(0x200) (`docs/re/R1_vtable.md`). We can feed gaze to engine code that queries it, but foveation itself lives in
our Vulkan layer.

#### Option 1 (recommended default): VRS foveation via a shading-rate attachment

1. **Device creation:** enable `VkPhysicalDeviceFragmentShadingRateFeaturesKHR{pipeline, primitive?, attachment}`
   and `VK_KHR_create_renderpass2`. We own device creation through `xrCreateVulkanDeviceKHR`.
2. **Rate image:** per eye, `R8_UINT`, size `ceil(eyeW/16) × ceil(eyeH/16)`. Use an array of 2 layers for the
   multiview path, or one double-wide image for the double-wide path.
3. **Fill it with a compute shader each frame.** Rings around a centre point per eye: 1×1 inside about 15–20°,
   1×2/2×1, then 2×2, then 2×4 / 4×4 at the edge. Optionally go coarser outside the `XR_KHR_visibility_mask`
   hidden area.
   - **Fixed** centre: the projection centre of each eye. Account for asymmetric FOV: centre = NDC of the forward
     axis, not the image centre.
   - **Eye-tracked** centre: get the `XR_EXT_eye_gaze_interaction` gaze pose at the predicted display time, then
     intersect the gaze ray with each eye's projection to get per-eye NDC. Smooth it, add a margin that grows with
     gaze velocity, and fall back to fixed when `isActive` is false or no sample arrived within ~50 ms.
4. **Render-pass hook:** attach the rate image to the scene render passes via
   `VkFragmentShadingRateAttachmentInfoKHR` in `vkCreateRenderPass2` / dynamic rendering. Do this at the same hook
   where we inject `viewMask`, so it applies only to main-view geometry passes (depth prepass, opaque, translucent).
   Keep it off shadows, UI and post-processing.
5. **Combiner:** hook `vkCmdSetFragmentShadingRateKHR`, which the engine calls itself. Force
   `combinerOps = {KEEP, MAX}` so the engine's own per-draw coarse rate and our foveation rate combine without
   either overriding the other. When the engine's VRS is off, set it ourselves.
6. **Interaction with DLSS/FSR:** apply VRS to the *render-resolution* input, and keep peripheral rates moderate
   (≤2×2). Upscalers amplify blockiness, and the fovea must stay 1×1.

**Why VRS is the default:**
- It works on every runtime (Monado, SteamVR PC, streamed Frame) because it is invisible to OpenXR.
- It needs no extra views, draws or compositor support.
- It costs nothing on the CPU, which matters for SS4's draw-call-heavy hordes.
- It is compatible with screen-space post-processing.
- Gains are pixel-shader only (no raster/geometry/bandwidth savings). Expect roughly 10–30% GPU in shading-bound
  scenes [inference; measure in `docs/PERF.md`].

#### Option 2: quad views (`PRIMARY_STEREO_WITH_FOVEATED_INSET` / `XR_VARJO_quad_views` + `XR_VARJO_foveated_rendering`)

**Spec** [verified, [XrViewConfigurationType](https://registry.khronos.org/OpenXR/specs/1.1/man/html/XrViewConfigurationType.html)]:
- `PRIMARY_QUAD_VARJO` is an alias of OpenXR 1.1's `PRIMARY_STEREO_WITH_FOVEATED_INSET` (1000037000).
- View order: 0 left wide, 1 right wide, 2 left inset, 3 right inset.
- Each inset must lie within its wide view. The runtime may blend the edges, so the app **must still render the
  inner FOV in the wide view**.
- Inset `fov` may change, and its `pose` must equal the stereo view's pose.
- `XR_VARJO_foveated_rendering` (requires quad views) adds:
  - `XrSystemFoveatedRenderingPropertiesVARJO`;
  - `XrFoveatedViewConfigurationViewVARJO` (inset resolution with foveation active), chained to
    `xrEnumerateViewConfigurationViews`;
  - `XrViewLocateFoveatedRenderingVARJO`, chained to `xrLocateViews`, which returns gaze-following inset FOVs;
  - `XR_REFERENCE_SPACE_TYPE_COMBINED_EYE_VARJO`.
  - Man page: [XR_VARJO_foveated_rendering](https://registry.khronos.org/OpenXR/specs/1.1/man/html/XR_VARJO_foveated_rendering.html).

**Savings** [reported, [QVF wiki](https://github.com/mbucchia/Quad-Views-Foveated/wiki/What-is-Quad-Views-rendering%3F)]:
- Quest Pro: about 46% of the stereo pixel count. Varjo Aero: about 38%.
- The saving covers shading, raster *and* post-processing.
- Costs: geometry in the overlap is drawn twice (more CPU), and screen-space effects are "incompatible" in places.
  Varjo recommends disabling bloom, DoF, motion blur, SSAO, SSR and film grain.

**Runtime support for our targets:**

| Runtime | Quad views? | Who composites the inset? | Gaze-following inset? |
|---|---|---|---|
| Monado (current builds) | Yes, emulated on stereo HMDs | **Runtime** (compute compositor, edge blend) | No `XR_VARJO_foveated_rendering`. App may move the inset itself (fovMutable) [inference] |
| SteamVR PC (2.19, incl. streamed Frame) | **No** | **App** must composite the inset into a stereo swapchain | App computes from `XR_EXT_eye_gaze_interaction` |
| Varjo / Pimax (Windows) | Native | Runtime | Native |

- **mbucchia's Quad-Views-Foveated API layer** (MIT, last release 1.1.3 in 2023, last commit July 2024)
  ([GitHub](https://github.com/mbucchia/Quad-Views-Foveated)) is not usable here:
  - It emulates the Varjo extensions and composites "each quad view projection layer into a stereo projection layer".
  - Its edge smoothing and CAS sharpening cost "below 200 microseconds".
  - It drives the inset from `XR_EXT_eye_gaze_interaction` or `XR_FB_eye_tracking_social`.
  - Per-runtime settings (`settings.cfg`): fixed fallback 0.5×0.45 of the FOV, Oculus 0.4 peripheral / 1.1 focus
    multipliers, and "Turbo Mode causes unexplained errors with SteamVR".
  - Its source says "We only support D3D11 at the moment" and bypasses the layer for non-D3D11 apps
    (`layer.cpp`, `m_bypassApiLayer = !m_requestedD3D11`). It is also Windows-only.
  - Under Proton the app-side API is translated to Vulkan anyway, and the layer would have to sit on the Windows
    loader side.
  - **It cannot serve our Vulkan app.** On SteamVR we would reimplement its composition step ourselves (one compute
    pass: upsample the wide view into the stereo swapchain, blend the inset with a feathered edge).

**Engine cost for quad views** [inference from the embedded shader source]:
- The engine's single-pass stereo derives the right eye from the left: `VRRightMatrix(c_mViewProjLeft,
  c_vViewProjRightX)` changes only the X row, and `VRSelect`/`VRSelectPosX_VS` pick between two variants.
- An inset has a **different projection**, not an X-shifted one. Quad views therefore need either:
  - (a) a second full scene submission for the inset pair. That breaks our rule "never run ... draw twice per frame
    for stereo" (`AGENTS.md`) and doubles CPU submission in horde scenes; or
  - (b) a 4-view multiview pass (`viewMask=0b1111`, `maxMultiviewViewCount=32` on this GPU). That requires replacing
    `VRRightMatrix`/`VRSelect` with full per-view matrices indexed by `SV_ViewID` in every hooked shader (our DXC
    hook already rewrites `GetVREye()`).
- Also needed: per-view visibility culling widened to the union, and every screen-space effect (SSAO, bloom, DoF,
  motion blur, the drawport rescale) run per view. Mismatches between inset and context show as seams.
- With an upscaler, quad views need **4 DLSS/FSR instances** with independent histories, and a moving inset
  invalidates inset history (re-projection of a moving crop). That is solvable but noisy.

#### Cooperating with Steam Frame foveated streaming
- **Nothing is required from the app.** Submit the normal `PRIMARY_STEREO` projection layer at the runtime's
  recommended resolution, with accurate per-eye `fov`/`pose` from `xrLocateViews` for that frame.
- Avoid conflicts:
  - Use the **same gaze source** (`XR_EXT_eye_gaze_interaction`) for VRS so our full-rate region coincides with the
    streamer's high-bitrate region.
  - Don't bake a fixed foveation centre that disagrees with the eye-tracked encoder region when gaze is available.
  - Keep the foveal VRS region ≥ the streamer's focus area (be generous: ~20° radius plus a gaze-velocity margin).
  - Don't submit quad views on SteamVR: it can't consume them; composite to stereo if quad views are ever done.
- Low-detail VRS periphery should also encode cheaply. This is not measured [inference].

---

## Recommendations and next steps

1. **R-item: upscaler insertion point.**
   - In RenderDoc under Proton (Vulkan), locate the HDR scene target before bloom/tonemap and the `RescalePS`
     pass.
   - Confirm whether `ren_fMax3DRenderingMPix` / `vr_fRenderingPixelRatio` shrink the 3D drawport *before*
     post-processing. If so, swapping `RescalePS` for a temporal upscaler is a fallback (post-tonemap; worse quality).
   - Record addresses in `docs/re/builds/<sha>.json`.
2. **R-item: motion vectors.**
   - Find what sets `GENERATE_MVECS=1` and which render target receives the `.gb` velocity of the depth prepass.
   - Check the right-eye previous matrix.
   - If it can't be made per-eye, implement camera-only MVs from depth (compute) with per-eye previous view-proj.
3. **Jitter + LOD bias** in `src/se4`:
   - Halton(2,3) with phases = 8·(display/render)².
   - Sub-pixel offset applied to each eye's projection after `xrLocateViews`; unjittered matrices kept for MVs.
   - `vkCreateSampler` mip bias.
   - Reset on level load, cutscene and menu.
4. **Upscaler backend interface:**
   - FSR 3.1 Vulkan first (vendor-neutral, two contexts).
   - DLSS SR/DLAA second: NGX Vulkan, two handles, subrects for double-wide. Query NGX instance/device extension
     requirements before `xrCreateVulkan*KHR`. Ship `nvngx_dlss.dll`. Disable OTA.
   - Proton needs no env vars on current builds; `nvngx.dll`/`_nvngx.dll` are already in the 257420 prefix.
   - Optional: XeSS 2.
   - Offer DLAA/FSR-native-AA as an AA mode even at 1.0 scale.
5. **Foveation, default = VRS attachment:**
   - Fixed foveation everywhere.
   - Eye-tracked when `XR_EXT_eye_gaze_interaction` reports support (SteamVR/streamed Frame, Monado with an
     eye-tracking driver).
   - Hook `vkCmdSetFragmentShadingRateKHR` combiner ops.
   - Add to the perf matrix: VRS off / fixed / eye-tracked × upscaler off / FSR / DLSS.
6. **Quad views: not now.** Revisit only if the perf matrix shows GPU-bound pixel cost dominating and VRS isn't
   enough. Prototype order:
   - First, Monado's runtime-composited quad views with a 4-view multiview pass (requires full per-view matrices in
     the DXC hook). Test whether a gaze-moved inset `fov` is honoured (fovMutable).
   - Then app-side composition for SteamVR.
   - Do not plan on mbucchia's layer: D3D11-only, Windows-only.
7. **Probe update** (`vk_xr_probe.exe`):
   - Log `fovMutable`, the view configuration list (look for 1000037000), `supportsEyeGazeInteraction`, and the
     FB/META/VARJO extension presence on Monado (Envision) and SteamVR (PC, plus the Frame when available).
   - Log the `VkPhysicalDeviceFragmentShadingRate*` properties and the NGX extension requirements.
   - Store results next to R5.
