# Gate R5 — graphics interop: **PASS** (Proton, simulated Monado)

`vk_xr_probe.exe` (src/probe) ran inside the SS4 prefix (compatdata/257420, GE-Proton11-9) through
`proton run` (steam.exe helper initialises the VR bridge), wineopenxr → host loader → private Monado
25.1.0 with the `remote` driver (`lab/monado/config`, port 42431).

Environment: `XR_RUNTIME_JSON=<monado>/share/openxr/1/openxr_monado.json`,
`VR_OVERRIDE=~/.local/share/envision/ovr_comp/target/release` (XRizer; needed for Proton to bring up
the bridge), `PRESSURE_VESSEL_IMPORT_OPENXR_1_RUNTIMES=1` when going through Steam. monado-service exits
when stdin closes; keep stdin open (`tail -f /dev/null | monado-service`).

Results:
- `XR_KHR_vulkan_enable2` present; also visibility_mask, composition_layer_cylinder/depth, hand_tracking,
  eye_gaze_interaction, local_floor, display_refresh_rate.
- `xrCreateVulkanInstanceKHR` / `xrCreateVulkanDeviceKHR` succeed; device NVIDIA RTX 4090, Vulkan 1.4.351.
- Device: `VK_KHR_multiview` feature on, `VK_KHR_fragment_shading_rate`, `VK_KHR_create_renderpass2`,
  `VK_NV_viewport_array2` available.
- Swapchain 1344x1512 x 2 array layers, format 43 (R8G8B8A8_SRGB), 3 images.
- Visibility mask (hidden triangle mesh) per eye: OK.
- Session IDLE → READY → SYNCHRONIZED → VISIBLE → FOCUSED; 300 frames, per-eye layer clears submitted.

The game itself runs on Vulkan with `+gfx_strAPI Vulkan` (log: `[VLK]`, Vulkan 1.4.351, immediate
present, separate transfer queue). Add `+prj_strDisabledCompatibilityCheckMessages GPUDetect` to skip
the blocking GPUDetect dialog on this GPU/driver.
Still to verify on hardware: the same probe on Envision/Monado with the Beyond, and on SteamVR.
