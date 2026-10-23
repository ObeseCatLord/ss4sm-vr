# Lab notes

- Launch: `tools/lab/run_sam4.sh [timeout] [+cvar ...]` (Steam `-applaunch`, Vulkan, GPUDetect dialog off).
- Kill: `tools/lab/killgame.sh` (matches Wine argv[0]; never `pkill -f Sam4.exe`, which matches the shell).
- Payload lab install: `Bin/x64/msimg32.dll`, `Bin/x64/ss4smvr/ss4smvr.dll`, prefix override
  `HKCU\Software\Wine\DllOverrides msimg32=native,builtin` (set with `proton runinprefix reg add`).
- **Never create threads in DllMain / the attach call during process start**: on Wine the game stalls
  before writing its log. Bisected with `ss4smvr/debug_skip.txt`; MinHook init and
  LdrRegisterDllNotification are fine. Threads start from the vulkan-1.dll load notification.
- Simulated Monado: `tail -f /dev/null | XDG_CONFIG_HOME=lab/monado/config P_OVERRIDE_ACTIVE_CONFIG=remote ...
  ~/ss2-vr-private/monado-exact-prefix/bin/monado-service` (exits when stdin closes).
