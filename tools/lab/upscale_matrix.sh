#!/bin/bash
# Lab: image stability/sharpness of the upscaler options on a still camera in Pompeii (SS4), from the headset view
# (left eye of the lab Monado window).
# usage: upscale_matrix.sh <out-dir> "<options line>" ...   e.g. "upscaler=0" "upscaler=2 upscale_lod_bias=0"
HERE="$(cd "$(dirname "$0")" && pwd)"; ROOT="$(dirname "$(dirname "$HERE")")"
. "$HERE/paths.sh" ss4; G="$LAB_GAME"
OUT=$1; shift; mkdir -p "$OUT"
for cfg in "$@"; do
  tag=$(echo "$cfg" | tr ' =' '_-')
  { echo lab_giveall=1; for kv in $cfg; do echo "$kv"; done; } > "$G/Bin/x64/ss4smvr/options.txt"
  "$HERE/level_test.sh" ss4 Content/SeriousSam4/Levels/01_PB/02_Pompeii.wld "$OUT/$tag" >/dev/null 2>&1
  "$HERE/until_player.sh" ss4 400 >/dev/null || { echo "$tag: no player"; continue; }
  "$ROOT/lab/monado/input_driver" head=0,1.6,0,0,0,0 lpos=-0.25,0.6,-0.1 rpos=0.25,0.6,-0.1 >/dev/null; sleep 6
  for i in 1 2 3 4 5 6; do
    "$HERE/capture.sh" Monado "$OUT/$tag-full$i.png" 100 >/dev/null 2>&1
    python3 -c "from PIL import Image; im=Image.open('$OUT/$tag-full$i.png'); w,h=im.size; im.crop((0,0,w//2,h)).save('$OUT/$tag-s$i.png')"
    sleep 0.07
  done
  python3 -I "$HERE/image_stats.py" "$OUT/$tag"-s?.png | sed "s/^/$tag: /"
done
"$HERE/stop_lab.sh" >/dev/null 2>&1
