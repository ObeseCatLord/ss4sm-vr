# Lab notes

- **All lab runs use a private display** (user request 2026-10-23): `tools/lab/private_display.sh` starts a
  headless Weston (gl renderer, 1920x1080@90) with Xwayland and prints its X display; game and Monado
  windows live only there. `tools/lab/capture.sh <title> <out.png> [resize%]` captures windows from it.
- Launch: `tools/lab/run_game.sh ss4|sm [timeout] [+cvar ...]` — direct `proton run`, private display,
  windowed 1280x720, Vulkan, muted (`sfx_fMasterVolume 0`), GPUDetect dialog off, OpenXR env (XR=0 to skip).
  Settings ini (`Steam/userdata/<id>/<app>/local/SeriousSam4.ini`) is backed up before runs.
- Simulated Monado: `tools/lab/monado.sh` (remote driver on the private display).
- Kill: `tools/lab/killgame.sh` (matches Wine argv[0]; never `pkill -f Sam4.exe`, which matches the shell).
- The GetProcAddress hook (game-module callers only) replaces the earlier loader-notification + export
  patching, which intermittently stalled startup when vulkan-1.dll loaded. Callbacks under the loader
  lock must not call the CRT.
- Payload lab install: `Bin/x64/msimg32.dll`, `Bin/x64/ss4smvr/ss4smvr.dll`, prefix override
  `HKCU\Software\Wine\DllOverrides msimg32=native,builtin` (set with `proton runinprefix reg add`).
- **Never create threads in DllMain / the attach call during process start**: on Wine the game stalls
  before writing its log. Bisected with `ss4smvr/debug_skip.txt`; MinHook init and
  LdrRegisterDllNotification are fine. Threads start from the vulkan-1.dll load notification.
- monado-service exits when stdin closes; the script keeps stdin open.

- **Never stop the user's runtime.** On 2026-10-23 the old `monado.sh` ran `pkill -x monado-service` and
  stopped the user's Envision Monado. `tools/lab/monado.sh` now only stops the process in its own pid file
  when its exe is the private lab build, and refuses to start while any other Monado is running.
