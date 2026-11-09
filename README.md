# SS4SM-VR

A 6DoF OpenXR VR mod for **Serious Sam 4** (Steam build 616154) and **Serious Sam: Siberian Mayhem** (build 628492).

Both games still ship most of the VR gameplay layer from Serious Sam Fusion 2017's VR mode, but without its headset
device. The mod adds an OpenXR device in its place and drives the engine's own VR code. It adds stereo rendering,
tracked hands and weapons, and the parts the games no longer have.

> **Status: development build.** Tested in a simulated OpenXR runtime (Monado) on Linux/Proton. Testing on a real
> headset is still to come. Expect rough edges and report them (see *Logs* below).

## Features

- **Stereo and head tracking:** two-pass or single-pass stereo, the menu on a VR panel, and a desktop mirror window.
- **Hands and weapons:**
  - Weapons follow the controllers; shots leave the muzzle.
  - Dual wielding and two-hand grips.
  - Scopes with a magnified eyepiece view.
  - One crosshair per weapon, drawn at the aim point.
- **Ammo counters on the guns** as in Fusion 2017's VR mode, drawn in the scene so your hand can hide them.
- **The game's own weapon and gadget wheels**, held at the hand. Gadgets are thrown by hand.
- **Melee:**
  - Physical swings.
  - A swing at an enemy that shows the melee prompt starts the game's own melee-kill animation.
- **Vehicles:** thumbstick driving, or grabbing the vehicle's own steering wheel or handlebars.
- **Upscaling:** DLSS (NVIDIA) and FSR 3.1 in the eye render, with sharpening and a per-eye render resolution.
- **Foveated rendering:** eye-tracked on headsets that report eye gaze (Steam Frame, Quest Pro, ...), fixed
  otherwise, using variable-rate shading.
- **Options menu:**
  - Options > **VR Controls** sets turning (smooth or snap, speed or angle, which stick), the comfort vignette and the
    ammo counters.
  - Options > Graphics sets the upscaler, render resolution and foveation.
- **Multiplayer:** VR players' head and hands are sent to others. Desktop players can join if they also install the
  mod (desktop mode).

Not supported: teleport locomotion. The mod moves you smoothly with the stick.

## Requirements

- Serious Sam 4 and/or Siberian Mayhem from Steam, current builds (616154 / 628492). Other builds are refused and
  the game stays flat.
- An OpenXR runtime: SteamVR, Monado / Envision (Linux), or Oculus / Virtual Desktop / WMR (Windows).
- Vulkan. The mod switches the game to Vulkan in memory and never changes your settings file.
- DLSS needs an NVIDIA RTX GPU. FSR 3.1 works on any GPU.

## Install

1. Download `ss4sm-vr-<version>.zip` from [Releases](../../releases), or from the latest
   [build](../../actions/workflows/build.yml) artifact.
2. Extract it into the game folder, for example `.../steamapps/common/Serious Sam 4/`, so that
   `Bin/x64/msimg32.dll` sits next to `Sam4.exe`. The mod only adds files; no game file is replaced.
3. **Linux / Proton:** set the Steam launch options to

   ```
   WINEDLLOVERRIDES="msimg32=n,b" PRESSURE_VESSEL_IMPORT_OPENXR_1_RUNTIMES=1 %command%
   ```

   **Windows:** no launch option is needed.
4. Start your OpenXR runtime, then start the game.

`Bin/x64/ss4smvr/enable_vr` turns VR on. Without it the mod stays inactive. For a **desktop player** who wants to
join VR players, delete `enable_vr` and create `Bin/x64/ss4smvr/options.txt` containing:

```
mp_guards=1
desktop_upscaler=1
```

From a source checkout, `tools/install.py install "<game dir>" [--mode vr|desktop]` does the same. It checks the
game build, keeps a receipt, and `tools/install.py uninstall "<game dir>"` removes exactly what it added.

## Uninstall

Delete `Bin/x64/msimg32.dll`, `Bin/x64/nvngx_dlss.dll` and the `Bin/x64/ss4smvr/` folder. Then remove the launch
option.

## Options and logs

- The VR settings are in the game: Options > **VR Controls** and Options > **Graphics**. The mod saves them in
  `Bin/x64/ss4smvr/upscaler_menu.txt`.
- Advanced switches go in `Bin/x64/ss4smvr/options.txt`, one `key=value` per line. They are documented in
  [docs/USER_VR_TEST.md](docs/USER_VR_TEST.md).
- Log: `Bin/x64/ss4smvr/ss4smvr.log`. Please attach it to bug reports, together with the game's own log
  (`Log/Sam4.log` or `Log/SamSM.log`).

## Building

The build cross-compiles with MinGW-w64 (GCC, posix threads) and CMake on Linux. The
[workflow](.github/workflows/build.yml) is the reference build: Arch Linux with `mingw-w64-gcc`, `shaderc`, `cmake`
and `ninja`.

```bash
cmake -S . -B build-win -G Ninja -DCMAKE_TOOLCHAIN_FILE=cmake/mingw-w64-x86_64.cmake \
  -DVULKAN_HEADERS_ROOT=<Vulkan-Headers v1.4.363>/include \
  -DDLSS_SDK=<NVIDIA DLSS SDK v310.9.16> -DFFX_SDK=<AMD FidelityFX SDK v1.1.4>
cmake --build build-win
cmake -S . -B build-tests -DSS4SMVR_TESTS=ON && cmake --build build-tests && ctest --test-dir build-tests
```

OpenXR-SDK and MinHook are fetched at pinned commits.

Reverse-engineering notes are in [docs/re](docs/re). Every engine address is recorded per build and verified unique
in all four executables.

## Credits

- Croteam, for Serious Sam 4, Siberian Mayhem and the VR code from Fusion 2017 that is still inside them.
- Khronos (OpenXR loader), Tsuda Kageyu (MinHook), NVIDIA (DLSS SDK), AMD (FidelityFX SDK). See
  [NOTICE.md](NOTICE.md).
- The SS2 VR mod (same author): shared maths, comfort and controls helpers.
- **AI disclosure:** this mod was written largely by Claude (Anthropic) in Claude Code, directed and tested by
  ObeseCatLord.

## License

GPL-3.0 ([LICENSE](LICENSE)), with an additional permission (GPL-3.0 §7) for the NVIDIA DLSS SDK parts. See
[NOTICE.md](NOTICE.md). Not affiliated with or endorsed by Croteam or Devolver Digital.
