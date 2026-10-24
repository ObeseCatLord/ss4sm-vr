#!/bin/bash
# Run Sam4.exe directly through GE-Proton with the OpenXR environment (simulated or real Monado).
# usage: run_sam4_xr.sh [timeout_s] [extra +cvar args...]   XR_RUNTIME_JSON may be overridden by the caller.
GAME="/home/obesecatlord/FasterGames/SteamLibrary/steamapps/common/Serious Sam 4"
TO=${1:-90}; shift
"$(dirname "$0")/killgame.sh" >/dev/null; sleep 2
export STEAM_COMPAT_DATA_PATH=/home/obesecatlord/FasterGames/SteamLibrary/steamapps/compatdata/257420
export STEAM_COMPAT_CLIENT_INSTALL_PATH=$HOME/.local/share/Steam
export SteamAppId=257420 SteamGameId=257420
export XR_RUNTIME_JSON=${XR_RUNTIME_JSON:-$HOME/ss2-vr-private/monado-exact-prefix/share/openxr/1/openxr_monado.json}
export VR_OVERRIDE=${VR_OVERRIDE:-$HOME/.local/share/envision/ovr_comp/target/release}
export PROTON_LOG=0 WINEDEBUG=-all
T=$(date +%s)
cd "$GAME/Bin/x64"
setsid "$HOME/.local/share/Steam/compatibilitytools.d/GE-Proton11-9-x86_64/proton" run ./Sam4.exe +gfx_strAPI Vulkan \
  +prj_strDisabledCompatibilityCheckMessages GPUDetect "$@" > /tmp/claude-1000/sam4_proton.out 2>&1 < /dev/null &
for ((i=0;i<TO;i+=2)); do
  if [ "$(stat -c %Y "$GAME/Log/Sam4.log")" -gt "$T" ] && grep -q 'User interaction stage reached' "$GAME/Log/Sam4.log"; then echo "MENU after ${i}s"; exit 0; fi
  sleep 2
done
echo "TIMEOUT (no menu in ${TO}s)"; exit 1
