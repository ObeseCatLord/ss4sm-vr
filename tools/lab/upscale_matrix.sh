#!/bin/bash
# Lab: image stability/sharpness of the upscaler options on a still camera in Pompeii (SS4).
# usage: upscale_matrix.sh <out-dir> "<options line>" ...   e.g. "upscaler=0" "upscaler=2 upscale_jitter_x=-1"
HERE="$(cd "$(dirname "$0")" && pwd)"; ROOT="$(dirname "$(dirname "$HERE")")"
. "$HERE/paths.sh" ss4; G="$LAB_GAME"
OUT=$1; shift; mkdir -p "$OUT"
for cfg in "$@"; do
  kill -0 "$(cat "$ROOT/lab/monado/monado.pid" 2>/dev/null)" 2>/dev/null || { echo "lab Monado is not running - stopping"; exit 3; }
  tag=$(echo "$cfg" | tr ' =' '_-')
  "$HERE/killgame.sh" >/dev/null 2>&1
  { echo lab_giveall=1; for kv in $cfg; do echo "$kv"; done; } > "$G/Bin/x64/ss4smvr/options.txt"
  "$HERE/level_test.sh" ss4 Content/SeriousSam4/Levels/01_PB/02_Pompeii.wld "$OUT/$tag" >/dev/null 2>&1
  "$HERE/until_player.sh" ss4 400 >/dev/null || { echo "$tag: no player"; continue; }
  "$ROOT/lab/monado/input_driver" head=0,1.6,0,0,0,0 >/dev/null; sleep 6
  for i in 1 2 3 4 5 6; do "$HERE/capture.sh" "Serious Sam 4" "$OUT/$tag-s$i.png" 100 >/dev/null 2>&1; sleep 0.07; done
  python3 -I "$HERE/image_stats.py" "$OUT/$tag"-s?.png | sed "s/^/$tag: /"
done
"$HERE/killgame.sh" >/dev/null 2>&1
