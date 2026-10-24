#!/bin/bash
# Lab: in a running level with enemies near (e.g. SV_Backyard in Survival), find the knife on the right hand's wheel
# (8 directions), swing it across the front, then kick with the right B. Prints the mod's melee / use log lines.
# usage: melee_lab.sh ss4|sm <out-prefix>
HERE="$(cd "$(dirname "$0")" && pwd)"; ROOT="$(dirname "$(dirname "$HERE")")"; ID="$ROOT/lab/monado/input_driver"
DIR="Serious Sam 4"; [ "$1" = sm ] && DIR="Siberian Mayhem"
L="/home/obesecatlord/FasterGames/SteamLibrary/steamapps/common/$DIR/Bin/x64/ss4smvr/ss4smvr.log"
OUT=$2
H="head=0,1.6,0,0,0,0"; LH="lpos=-0.2,1.3,-0.3"; RX=0.25; RY=1.3; RZ=-0.35
start=$(wc -l < "$L")
found=0
for d in "-0.42 -0.91" "0 1" "0.7 0.7" "1 0" "0.7 -0.7" "0 -1" "-0.7 -0.7" "-1 0" "-0.7 0.7"; do
  set -- $d
  x=$(awk "BEGIN{print $RX + $1 * 0.09}"); y=$(awk "BEGIN{print $RY + $2 * 0.09}")
  $ID $H $LH rpos=$RX,$RY,$RZ rforce=1 >/dev/null; sleep 0.6
  $ID $H $LH rpos=$x,$y,$RZ rforce=1 >/dev/null; sleep 0.8
  $ID $H $LH rpos=$x,$y,$RZ >/dev/null; sleep 0.3; $ID $H $LH rpos=$RX,$RY,$RZ >/dev/null; sleep 1.5
  if tail -n +"$start" "$L" | grep -q "melee: knife in hand"; then echo "knife at direction $d"; found=1; break; fi
done
[ $found = 1 ] || echo "knife not found on the right wheel"
"$HERE/capture.sh" Monado "$OUT-knife.png" 50 >/dev/null 2>&1
for k in 1 2 3 4 5 6; do  # swings: right to left across the front, then back
  for x in 0.45 0.3 0.15 0.0 -0.15 -0.3; do $ID $H $LH rpos=$x,1.35,-0.5 >/dev/null; done
  sleep 0.3
  for x in -0.3 -0.15 0.0 0.15 0.3 0.45; do $ID $H $LH rpos=$x,1.25,-0.5 >/dev/null; done
  sleep 0.3
done
"$HERE/capture.sh" Monado "$OUT-swung.png" 50 >/dev/null 2>&1
for k in 1 2 3 4 5 6; do $ID $H $LH rpos=$RX,$RY,$RZ rb=1 >/dev/null; sleep 0.3; $ID $H $LH rpos=$RX,$RY,$RZ >/dev/null; sleep 0.8; done
"$HERE/capture.sh" Monado "$OUT-kicked.png" 50 >/dev/null 2>&1
tail -n +"$start" "$L" | tr -d '\r' | grep -E "melee:|right B|native VR use" | sort | uniq -c | sort -rn | head -20
