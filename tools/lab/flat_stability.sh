#!/bin/bash
# Lab: still-camera image stability of the desktop (flat) upscaler in Pompeii (SS4), lab copy, private display.
# usage: flat_stability.sh <out-dir> "<options line>[;<+cvars>]" ...
#   e.g. "upscaler=0" "upscaler=2" "upscaler=2;+sha_iAllowTreeSwaying 0" "upscaler=2 upscale_lod_bias=0"
# Each config: a fresh flat start of the level, Continue, the HUD, SETTLE (10) s, 10 captures of the game window, then
# image_stats.py (temporal diff over the upper-centre region: trees and bushes, no HUD or weapon).
HERE="$(cd "$(dirname "$0")" && pwd)"
. "$HERE/paths.sh" ss4; G="$LAB_GAME"; L="$G/Log/Sam4.log"
LEVEL=Content/SeriousSam4/Levels/01_PB/02_Pompeii.wld
OUT=$1; shift; mkdir -p "$OUT"
for cfg in "$@"; do
  opts=${cfg%%;*}; cvars=""; [ "$cfg" != "$opts" ] && cvars=${cfg#*;}
  tag=$(echo "$cfg" | tr ' =;+' '_._-' | tr -s _)
  { echo desktop_upscaler=1; echo mp_guards=1; for kv in $opts; do echo "$kv"; done; } > "$G/Bin/x64/ss4smvr/options.txt"
  # shellcheck disable=SC2086
  VR=0 "$HERE/run_game.sh" ss4 150 +level "$LEVEL" $cvars >/dev/null || { echo "$tag: no menu"; continue; }
  timeout 240 bash -c "until grep -q \"Started simulation on '$LEVEL'\" \"$L\" || grep -q Crashed \"$L\"; do sleep 3; done"
  grep -q Crashed "$L" && { echo "$tag: CRASHED"; continue; }
  sleep 3
  python3 -I "$HERE/xclick.py" :3 "Serious Sam 4" >/dev/null
  python3 -I "$HERE/xkey.py" :3 Return
  # Pompeii's intro cutscene (no skip key in flat) ends 1.5-3 min after Continue: wait for the health HUD (green cross,
  # bottom left), then settle.
  hud=0
  [ "${HUDWAIT:-1}" = 0 ] && { sleep 200; hud=1; }  # debug views fool the HUD test: a fixed wait instead
  for ((t=0; t<360 && hud==0; t+=5)); do
    sleep 5
    "$HERE/capture.sh" "Serious Sam 4" "$OUT/.hud.png" >/dev/null 2>&1 || continue
    hud=$(python3 -I -c "
import numpy as np; from PIL import Image
a=np.asarray(Image.open('$OUT/.hud.png').convert('RGB')).astype(int); h,w=a.shape[:2]
r=a[h*660//720:h*700//720, w*15//1280:w*50//1280]
print(int(((r[...,1]>150)&(r[...,1]>r[...,0]+60)&(r[...,1]>r[...,2]+60)).sum()>40))")
    [ "$hud" = 1 ] && break
  done
  [ "$hud" = 1 ] || { echo "$tag: no gameplay HUD within 6 min"; continue; }
  sleep "${SETTLE:-10}"
  # MOVE=1: hold the strafe-right key (D) while capturing, for ghosting checks
  [ "${MOVE:-0}" = 1 ] && python3 -I -c "
import time
from Xlib import X, XK, display
from Xlib.ext import xtest
d = display.Display(':3'); c = d.keysym_to_keycode(XK.string_to_keysym('d'))
xtest.fake_input(d, X.KeyPress, c); d.sync()" 
  for i in $(seq -w 1 10); do "$HERE/capture.sh" "Serious Sam 4" "$OUT/$tag-f$i.png" >/dev/null 2>&1; done
  [ "${MOVE:-0}" = 1 ] && python3 -I -c "
from Xlib import X, XK, display
from Xlib.ext import xtest
d = display.Display(':3'); c = d.keysym_to_keycode(XK.string_to_keysym('d'))
xtest.fake_input(d, X.KeyRelease, c); d.sync()"
  python3 -I "$HERE/image_stats.py" "$OUT/$tag"-f??.png | sed "s/^/$tag: /"
  grep -E "dlss: flat|fsr: flat|foliage:" "$MODLOG" | tail -4 | sed "s/^/$tag:   /"
done
"$HERE/killgame.sh" >/dev/null 2>&1
