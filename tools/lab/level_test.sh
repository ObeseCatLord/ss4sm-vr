#!/bin/bash
# Lab: start a level in VR on the private display, wait for its simulation, press Continue, capture.
# usage: level_test.sh ss4|sm <level.wld> <out-prefix> [extra +cvars]   (lab Monado must be running)
HERE="$(cd "$(dirname "$0")" && pwd)"
G=ss4; [ "$1" = sm ] && G=sm
case "$G" in ss4) DIR="Serious Sam 4"; LOGF=Sam4.log; TITLE="Serious Sam 4";; sm) DIR="Siberian Mayhem"; LOGF=SamSM.log; TITLE="Serious Sam";; esac
LEVEL=$2; OUT=$3; shift 3
. "$HERE/paths.sh" "$G"; L="$LAB_GAME/Log/$LOGF"
# A fresh lab Monado per run: its remote driver dies with "stack smashing detected" after ~1000 input-driver
# connections (2026-10-24), and every button press is two connections.
"$HERE/killgame.sh" >/dev/null 2>&1
"$HERE/monado.sh" >/dev/null 2>&1 || { echo "lab monado failed"; exit 1; }
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
