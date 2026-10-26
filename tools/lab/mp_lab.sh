#!/bin/bash
# Lab: multiplayer on one machine with isolated lab copies (gbe_fork LAN, tools/lab/make_lab_tree.sh instances):
#   instance 2 = server (dedicated by default, flat, mod with desktop guards), instance 1 = VR client (lab Monado),
#   instance 3 = desktop client (flat, mod with guards; skip with DESKTOP=0).
# Prints the mod's channel lines (net:) and the games' connection lines; captures go to <out-dir>.
# usage: mp_lab.sh ss4|sm <out-dir> [dedicated|listen]   (listen: the server is a flat player instead)
HERE="$(cd "$(dirname "$0")" && pwd)"; ROOT="$(dirname "$(dirname "$HERE")")"; ID="$ROOT/lab/monado/input_driver"
G=$1; OUT=$2; MODE=${3:-dedicated}; mkdir -p "$OUT"
case "$G" in
  ss4) LEVEL=Content/SeriousSam4/Levels/01_PB/02_Pompeii.wld; LOGF=Sam4.log; TITLE="Serious Sam 4";;
  sm) LEVEL=Content/SeriousSamSM/Levels/01_SM/01_Refinery.wld; LOGF=SamSM.log; TITLE="Serious Sam";;
  *) echo "game?"; exit 2;;
esac
tree() { local n=$G; [ "$1" != 1 ] && n="$G-$1"; echo "$ROOT/lab/game/$n"; }
for i in 1 2 3; do [ -f "$(tree $i)/Bin/x64/steam_settings/steam_appid.txt" ] || "$HERE/make_lab_tree.sh" "$G" "$i" >/dev/null; done
opts() { printf '%s\n' "$@" > "$(tree "$INST")/Bin/x64/ss4smvr/options.txt"; }

"$HERE/stop_lab.sh" >/dev/null 2>&1
"$HERE/monado.sh" >/dev/null 2>&1 || { echo "lab monado failed"; exit 1; }

# Server (instance 2), flat.
INST=2; opts mp_guards=1 mp_log=1
SERVER_ARGS=(+prj_bStartAsServer 1 +prj_bLANServer 1 +gam_idGameMode Cooperative +gam_strLevel "$LEVEL" +gam_ctMaxPlayers 4)
[ "$MODE" = dedicated ] && SERVER_ARGS=(+gam_bDedicatedServer 1 "${SERVER_ARGS[@]}")
KEEP_RUNNING=1 VR=0 LAB_INSTANCE=2 "$HERE/run_game.sh" "$G" 180 "${SERVER_ARGS[@]}" | sed 's/^/server: /'
timeout 240 bash -c "until grep -q \"Started simulation on '$LEVEL'\" \"$(tree 2)/Log/$LOGF\"; do sleep 3; done" && echo "server: level running" \
  || echo "server: no level after 240 s"

# VR client (instance 1).
INST=1; opts lab_giveall=0 mp_log=1 native_vr_action=1
KEEP_RUNNING=1 LAB_INSTANCE=1 "$HERE/run_game.sh" "$G" 180 +gam_strConnectToIP 127.0.0.1 | sed 's/^/vr client: /'
# Desktop client (instance 3).
if [ "${DESKTOP:-1}" = 1 ]; then
  INST=3; opts mp_guards=1 mp_log=1
  KEEP_RUNNING=1 VR=0 LAB_INSTANCE=3 "$HERE/run_game.sh" "$G" 180 +gam_strConnectToIP 127.0.0.1 | sed 's/^/desktop client: /'
fi
sleep 60
"$HERE/until_player.sh" "$G" 300 | sed 's/^/vr client: /'
"$ID" head=0,1.6,0,0,0,0 lpos=-0.25,1.3,-0.35 rpos=0.25,1.3,-0.35 >/dev/null; sleep 20
"$HERE/capture.sh" Monado "$OUT/vr-client.png" 50 >/dev/null 2>&1
for i in 1 2 3; do
  t=$(tree $i); [ -d "$t" ] || continue
  echo "== instance $i ($(basename "$t"))"
  grep -a "net:\|attach: VR\|player_vr: .*desktop peer\|vrd_init" "$t/Bin/x64/ss4smvr/ss4smvr.log" 2>/dev/null | tr -d '\r' | tail -8
  grep -a -i "connect\|client\|server\|session\|Crashed\|Started simulation" "$t/Log/$LOGF" 2>/dev/null | tr -d '\r' | tail -6
done
echo "(leave running: inspect, then tools/lab/stop_lab.sh)"
