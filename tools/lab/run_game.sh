#!/bin/bash
# Lab launch on the PRIVATE display (never the user's desktop), muted, windowed 1280x720, Vulkan.
# usage: run_game.sh ss4|sm [timeout_s] [extra +cvar args...]
#   XR=0 to skip the OpenXR environment. Leaves the game running; stop with killgame.sh.
case "$1" in
  ss4) GAME="Serious Sam 4"; APP=257420; EXE=Sam4.exe; LOG=Sam4.log; INI=SeriousSam4.ini;;
  sm)  GAME="Siberian Mayhem"; APP=1792250; EXE=SamSM.exe; LOG=SamSM.log; INI=SeriousSamSM.ini;;
  *) echo "usage: $0 ss4|sm [timeout] [+cvars]"; exit 2;;
esac
shift; TO=${1:-90}; shift
HERE="$(cd "$(dirname "$0")" && pwd)"
# Never compete with the user's headset runtime: refuse while a non-lab Monado is running.
for p in $(pgrep -x monado-service); do
  if [ "$(readlink /proc/$p/exe)" != "$HOME/ss2-vr-private/monado-exact-prefix/bin/monado-service" ]; then
    echo "user's Monado is running (pid $p) - lab launch refused"; exit 3
  fi
done
ROOT=/home/obesecatlord/FasterGames/SteamLibrary/steamapps
G="$ROOT/common/$GAME"
"$HERE/killgame.sh" >/dev/null; sleep 2
D=$("$HERE/private_display.sh") || exit 1
# Settings snapshot: the game writes its ini on clean exit; keep the user's settings untouched.
for f in $HOME/.local/share/Steam/userdata/*/$APP/local/$INI; do [ -f "$f" ] && cp -n "$f" "$HERE/../../lab/$INI.user-backup"; done
unset WAYLAND_DISPLAY
export DISPLAY=$D
export STEAM_COMPAT_DATA_PATH=$ROOT/compatdata/$APP STEAM_COMPAT_CLIENT_INSTALL_PATH=$HOME/.local/share/Steam
export SteamAppId=$APP SteamGameId=$APP PROTON_LOG=0 WINEDEBUG=-all
export SS4SMVR_VR=1
export XDG_RUNTIME_DIR=/tmp/ss4vr-xr   # the lab Monado's private IPC directory (tools/lab/monado.sh)
if [ "${XR:-1}" = 1 ]; then
  export XR_RUNTIME_JSON=${XR_RUNTIME_JSON:-$HOME/ss2-vr-private/monado-exact-prefix/share/openxr/1/openxr_monado.json}
  export VR_OVERRIDE=${VR_OVERRIDE:-$HOME/.local/share/envision/ovr_comp/target/release}
fi
T=$(date +%s)
cd "$G/Bin/x64"
setsid "$HOME/.local/share/Steam/compatibilitytools.d/GE-Proton11-9-x86_64/proton" run ./$EXE +gfx_strAPI Vulkan \
  +prj_strDisabledCompatibilityCheckMessages GPUDetect +gfx_bFullScreen 0 +gfx_pixResWidth 1280 +gfx_pixResHeight 720 \
  +gfx_pixWinWidth 1280 +gfx_pixWinHeight 720 +sfx_fMasterVolume 0 +prj_bAutoSaveAtChapters 0 "$@" > "/tmp/claude-1000/${EXE%.exe}_proton.out" 2>&1 < /dev/null &
for ((i=0;i<TO;i+=2)); do
  if [ -f "$G/Log/$LOG" ] && [ "$(stat -c %Y "$G/Log/$LOG")" -gt "$T" ] && grep -q 'User interaction stage reached' "$G/Log/$LOG"; then echo "MENU after ${i}s (display $D)"; exit 0; fi
  sleep 2
done
echo "TIMEOUT (no menu in ${TO}s, display $D)"; exit 1
