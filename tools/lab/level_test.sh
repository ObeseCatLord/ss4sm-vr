#!/bin/bash
# Lab: start a level in VR on the private display, wait for its simulation, press Continue, capture.
# usage: level_test.sh ss4|sm <level.wld> <out-prefix> [extra +cvars]   (lab Monado must be running)
HERE="$(cd "$(dirname "$0")" && pwd)"
G=ss4; [ "$1" = sm ] && G=sm
case "$G" in ss4) DIR="Serious Sam 4"; LOGF=Sam4.log; TITLE="Serious Sam 4";; sm) DIR="Siberian Mayhem"; LOGF=SamSM.log; TITLE="Serious Sam";; esac
LEVEL=$2; OUT=$3; shift 3
L="/home/obesecatlord/FasterGames/SteamLibrary/steamapps/common/$DIR/Log/$LOGF"
"$HERE/run_game.sh" "$G" 150 +level "$LEVEL" "$@" || exit 1
timeout 240 bash -c "until grep -q \"Started simulation on '$LEVEL'\" \"$L\" || grep -q Crashed \"$L\"; do sleep 3; done"
grep -q Crashed "$L" && { echo CRASHED; exit 2; }
sleep 3
python3 -I "$HERE/xclick.py" :3 "$TITLE" >/dev/null
python3 -I "$HERE/xkey.py" :3 Return
sleep 8
"$HERE/capture.sh" Monado "$OUT-hmd.png" 50
"$HERE/capture.sh" "$TITLE" "$OUT-desktop.png" 50
echo "level running; captures $OUT-*.png"
