#!/bin/bash
# Lab launch on the PRIVATE display (never the user's desktop), muted, windowed 1280x720, Vulkan.
# usage: run_game.sh ss4|sm [timeout_s] [extra +cvar args...]
#   XR=0 to skip the OpenXR environment; VR=0 runs flat (no VR opt-in, no XR: multiplayer servers and desktop
#   clients). Leaves the game running; stop with killgame.sh.
#   Runs the isolated lab copy lab/game/<name> (tools/lab/make_lab_tree.sh: gbe_fork Steam emulator, its own saves,
#   logs and UserCfg) in its own Wine prefix lab/prefix/<name>: never the Steam install, its cloud saves or the
#   game's compatdata prefix. LAB_INSTANCE=2,3,... runs further copies (multiplayer clients/servers) side by side.
case "$1" in
  ss4) GAMEKEY=ss4; GAME="Serious Sam 4"; APP=257420; EXE=Sam4.exe; LOG=Sam4.log; INI=SeriousSam4.ini;;
  sm)  GAMEKEY=sm; GAME="Siberian Mayhem"; APP=1792250; EXE=SamSM.exe; LOG=SamSM.log; INI=SeriousSamSM.ini;;
  *) echo "usage: $0 ss4|sm [timeout] [+cvars]"; exit 2;;
esac
shift; TO=${1:-90}; shift
HERE="$(cd "$(dirname "$0")" && pwd)"
# The lab game talks only to the lab Monado (private XDG_RUNTIME_DIR + XR_RUNTIME_JSON below), so it may run
# while the user's own Monado is up; real-headset tests are saved for the end of the project (user, 2026-10-23).
LAB="$(cd "$HERE/../.." && pwd)/lab"
NAME=$GAMEKEY; [ "${LAB_INSTANCE:-1}" != 1 ] && NAME="$GAMEKEY-$LAB_INSTANCE"
G="$LAB/game/$NAME"
[ -f "$G/Bin/x64/steam_settings/steam_appid.txt" ] || "$HERE/make_lab_tree.sh" "$GAMEKEY" "${LAB_INSTANCE:-1}" >/dev/null || exit 1
"$HERE/install_lab_mod.sh" "$G" || exit 1
[ "${KEEP_RUNNING:-0}" = 1 ] || { "$HERE/killgame.sh" >/dev/null; sleep 2; }
D=$("$HERE/private_display.sh") || exit 1
unset WAYLAND_DISPLAY SteamAppId SteamGameId SteamClientLaunch
export DISPLAY=$D
mkdir -p "$LAB/prefix/$NAME"
export STEAM_COMPAT_DATA_PATH="$LAB/prefix/$NAME" STEAM_COMPAT_CLIENT_INSTALL_PATH=$HOME/.local/share/Steam
export WINEDLLOVERRIDES="msimg32=$([ "${NOMOD:-0}" = 1 ] && echo b || echo n,b)" PROTON_LOG=0 WINEDEBUG=-all  # NOMOD=1: the mod is not loaded
if [ "${VR:-1}" = 1 ]; then export SS4SMVR_VR=1; else unset SS4SMVR_VR; XR=0; fi
export XDG_RUNTIME_DIR=/tmp/ss4vr-xr   # the lab Monado's private IPC directory (tools/lab/monado.sh)
if [ "${XR:-1}" = 1 ]; then
  export XR_RUNTIME_JSON=${XR_RUNTIME_JSON:-$HOME/ss2-vr-private/monado-exact-prefix/share/openxr/1/openxr_monado.json}
  export VR_OVERRIDE=${VR_OVERRIDE:-$HOME/.local/share/envision/ovr_comp/target/release}
fi
T=$(date +%s)
cd "$G/Bin/x64"
setsid "$HOME/.local/share/Steam/compatibilitytools.d/GE-Proton11-9-x86_64/proton" run ./$EXE +gfx_strAPI Vulkan \
  +prj_strDisabledCompatibilityCheckMessages GPUDetect +gfx_bFullScreen 0 +gfx_pixResWidth 1280 +gfx_pixResHeight 720 \
  +gfx_pixWinWidth 1280 +gfx_pixWinHeight 720 +sfx_fMasterVolume 0 +prj_bAutoSaveAtChapters 0 "$@" > "$LAB/${NAME}_proton.out" 2>&1 < /dev/null &
echo $! > "$LAB/${NAME}.pid"
for ((i=0;i<TO;i+=2)); do
  if [ -f "$G/Log/$LOG" ] && [ "$(stat -c %Y "$G/Log/$LOG")" -gt "$T" ] && grep -q 'User interaction stage reached' "$G/Log/$LOG"; then echo "MENU after ${i}s (display $D)"; exit 0; fi
  sleep 2
done
echo "TIMEOUT (no menu in ${TO}s, display $D)"; exit 1
