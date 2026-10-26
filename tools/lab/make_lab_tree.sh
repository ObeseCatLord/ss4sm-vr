#!/bin/bash
# Build an isolated lab copy of a game: lab/game/<ss4|sm>[-<n>]/ mirrors the real install with symlinks, except
#   - Bin/x64/steam_api64.dll: the gbe_fork Steam emulator (offline, LAN multiplayer, no Steam client), with
#     Bin/x64/steam_settings/ (app id, a lab user, saves under this tree's GSE Saves/) - so lab runs never touch
#     Steam or the user's cloud saves and profile;
#   - Bin/x64/msimg32.dll + ss4smvr/: the mod from build-win (copies), with the lab's options.txt;
#   - Log/, Temp/ and UserCfg.lua: the tree's own (the game writes them in its root).
# Wine prefix: lab/prefix/<name> (tools/lab/run_game.sh), never the game's Steam compatdata prefix.
# usage: make_lab_tree.sh ss4|sm [instance-number]   (instance 2, 3, ... = more MP clients with their own users)
set -e
HERE="$(cd "$(dirname "$0")" && pwd)"; ROOT="$(dirname "$(dirname "$HERE")")"
case "$1" in
  ss4) DIR="Serious Sam 4"; APP=257420;;
  sm)  DIR="Siberian Mayhem"; APP=1792250;;
  *) echo "usage: $0 ss4|sm [n]"; exit 2;;
esac
N=${2:-1}; NAME=$1; [ "$N" != 1 ] && NAME="$1-$N"
REAL="/home/obesecatlord/FasterGames/SteamLibrary/steamapps/common/$DIR"
EMU="${GBE_DLL:-/home/obesecatlord/LargeGames/Games/Core Keeper/CoreKeeper_Data/Plugins/x86_64/steam_api64.dll}"
T="$ROOT/lab/game/$NAME"
mkdir -p "$T/Bin/x64" "$T/Log" "$T/Temp"
for e in "$REAL"/*; do
  b=$(basename "$e")
  case "$b" in Bin|Log|Temp|UserCfg.lua|UserCfg.lua.bak) continue;; esac
  ln -sfn "$e" "$T/$b"
done
for e in "$REAL/Bin/x64"/*; do
  b=$(basename "$e")
  case "$b" in steam_api64.dll|msimg32.dll|ss4smvr|*.dmp) continue;; esac
  case "$b" in
    Sam4.exe|SamSM.exe) rm -f "$T/Bin/x64/$b"; cp -f "$e" "$T/Bin/x64/$b";;  # a real file: the game takes its root from the exe path
    *) ln -sfn "$e" "$T/Bin/x64/$b";;
  esac
done
cp -f "$EMU" "$T/Bin/x64/steam_api64.dll"
S="$T/Bin/x64/steam_settings"; mkdir -p "$S"
echo $APP > "$S/steam_appid.txt"
cat > "$S/configs.user.ini" <<INI
[user::general]
account_name=SS4VR-Lab$N
account_steamid=$((76561197960287930 + N))
language=english
ip_country=US
[user::saves]
local_save_path=./
saves_folder_name=GSE Saves
INI
cat > "$S/configs.main.ini" <<INI
[main::connectivity]
disable_lan_only=0
disable_networking=0
listen_port=47584
offline=0
INI
cat > "$S/configs.app.ini" <<INI
[app::dlcs]
unlock_all=0
INI
"$HERE/install_lab_mod.sh" "$T"
echo "lab tree $T ready"
