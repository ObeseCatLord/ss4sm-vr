#!/bin/bash
# Lab: multiplayer on one machine with isolated lab copies (gbe_fork LAN, tools/lab/make_lab_tree.sh instances):
#   instance 2 = server (flat; dedicated by default), instance 1 = VR client (lab Monado), instance 3 = desktop client
#   (flat, mod with desktop guards; skip with DESKTOP=0). All connect over 127.0.0.1.
# The server is started from its console at the main menu (gamStartServer() + gam_* cvars + gamStart(); a
# command-line exec runs before the game's functions exist). Clients join with
# +gam_strConnectToIP. Prints the mod's channel lines (net:) and the games' connection lines; captures go to <out-dir>.
# usage: mp_lab.sh ss4|sm <out-dir> [dedicated|listen]   (listen: the server is a flat player too)
#   Leaves everything running for inspection; stop with tools/lab/stop_lab.sh.
HERE="$(cd "$(dirname "$0")" && pwd)"; ROOT="$(dirname "$(dirname "$HERE")")"; ID="$ROOT/lab/monado/input_driver"
G=$1; OUT=$2; MODE=${3:-dedicated}; mkdir -p "$OUT"
case "$G" in
  ss4) LEVEL=Content/SeriousSam4/Levels/01_PB/02_Pompeii.wld; LOGF=Sam4.log; TITLE="Serious Sam 4";;
  sm) LEVEL=Content/SeriousSamSM/Levels/01_SM/01_Refinery.wld; LOGF=SamSM.log; TITLE="Serious Sam";;
  *) echo "game?"; exit 2;;
esac
tree() { local n=$G; [ "$1" != 1 ] && n="$G-$1"; echo "$ROOT/lab/game/$n"; }
for i in 1 2 3; do [ -f "$(tree $i)/Bin/x64/steam_settings/steam_appid.txt" ] || "$HERE/make_lab_tree.sh" "$G" "$i" >/dev/null; done
opts() { printf '%s\n' "${@:2}" > "$(tree "$1")/Bin/x64/ss4smvr/options.txt"; }
focus() { python3 -I "$HERE/xfocus.py" :3 "$TITLE" "$(tree "$1")/Bin/x64" >/dev/null; }
key() { focus "$1" && python3 -I "$HERE/xkey.py" :3 "${@:2}"; }
waitlog() { timeout "$3" bash -c "until grep -aq \"$2\" \"$(tree "$1")/Log/$LOGF\"; do sleep 3; done"; }

"$HERE/stop_lab.sh" >/dev/null 2>&1

# Server (instance 2): flat, started from its console once the main menu is up.
opts 2 mp_guards=1 mp_log=1
SERVER_ARGS=(); [ "$MODE" = dedicated ] && SERVER_ARGS=(+gam_bDedicatedServer 1)
KEEP_RUNNING=1 VR=0 LAB_INSTANCE=2 "$HERE/run_game.sh" "$G" 180 "${SERVER_ARGS[@]}" | sed 's/^/server: /'
sleep 15; key 2 Return; sleep 20  # "press any key" -> main menu; let the menu settle (it resets gam_idGameMode as it opens)
key 2 F1; sleep 1.5
# gamStartServer() only prepares the server session (it starts nothing by itself); gamStart() then starts it with the
# gam_* cvars (lab 2026-10-26: gamStart() alone starts single player).
focus 2 && python3 -I "$HERE/xtype.py" :3 "gamStartServer(\"$LEVEL\", \"Cooperative\", \"Normal\", 4); gam_idGameMode = \"Cooperative\"; gam_strLevel = \"$LEVEL\"; gam_ctMaxPlayers = 4; gamStart()"
sleep 0.5; key 2 Return; sleep 1; key 2 F1
waitlog 2 'roundstart gamemode=.Cooperative' 300 && echo "server: cooperative game running" || { echo "server: no game after 300 s"; exit 1; }

# VR client (instance 1). The lab Monado starts now: left without a client for minutes it exits.
"$HERE/monado.sh" >/dev/null 2>&1 || { echo "lab monado failed"; exit 1; }
opts 1 lab_giveall=0 mp_log=1 native_vr_action=1
KEEP_RUNNING=1 LAB_INSTANCE=1 "$HERE/run_game.sh" "$G" 180 +gam_strConnectToIP 127.0.0.1 | sed 's/^/vr client: /'
waitlog 1 "Connection opened" 120 && echo "vr client: connected" || echo "vr client: no connection"
"$HERE/until_player.sh" "$G" 400 | sed 's/^/vr client: /'
# Desktop client (instance 3).
if [ "${DESKTOP:-1}" = 1 ]; then
  opts 3 mp_guards=1 mp_log=1
  KEEP_RUNNING=1 VR=0 LAB_INSTANCE=3 "$HERE/run_game.sh" "$G" 180 +gam_strConnectToIP 127.0.0.1 | sed 's/^/desktop client: /'
  waitlog 3 "Connection opened" 120 && echo "desktop client: connected" || echo "desktop client: no connection"
  for k in $(seq 1 30); do grep -aq "Started simulation on '$LEVEL'" "$(tree 3)/Log/$LOGF" && break; sleep 5; done
  for k in 1 2 3 4 5 6; do sleep 5; key 3 Return; done   # the loading screen's Continue
fi
"$ID" head=0,1.6,0,0,0,0 lpos=-0.25,1.3,-0.35 rpos=0.25,1.3,-0.35 >/dev/null; sleep 20
"$ID" head=0,1.6,0,0,0,0 lpos=-0.25,1.3,-0.35 rpos=0.25,1.6,-0.35 >/dev/null; sleep 3  # wave the right hand
"$HERE/capture.sh" Monado "$OUT/vr-client.png" 50 >/dev/null 2>&1
[ "${DESKTOP:-1}" = 1 ] && focus 3 && "$HERE/capture.sh" "$TITLE" "$OUT/desktop-client.png" 50 >/dev/null 2>&1
for i in 2 1 3; do
  t=$(tree $i); [ -d "$t" ] || continue
  echo "== instance $i ($(basename "$t"))"
  grep -a "net:\|attach: VR\|marked as playing" "$t/Bin/x64/ss4smvr/ss4smvr.log" 2>/dev/null | tr -d '\r' | tail -8
  grep -a -i "accepted connection\|Connection opened\|playerjoined\|disconnect\|Crashed\|Code guard" "$t/Log/$LOGF" 2>/dev/null | tr -d '\r' | tail -6
done
echo "(left running: inspect, then tools/lab/stop_lab.sh)"
