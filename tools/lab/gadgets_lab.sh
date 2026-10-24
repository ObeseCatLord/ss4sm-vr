#!/bin/bash
# Lab: in a running level with the local player (all gadgets given), take each gadget into the right hand with
# the left squeeze and set it off with its gesture (src/se4/gadgets.cpp): a throwing motion for throwables (the
# left hand points forward), the needle onto the left forearm, the bottle to the mouth, the chest, or the trigger.
# Then the right A with the wheel held cycles to the next gadget.
# usage: gadgets_lab.sh ss4|sm <out-prefix> [count]
HERE="$(cd "$(dirname "$0")" && pwd)"; ROOT="$(dirname "$(dirname "$HERE")")"; ID="$ROOT/lab/monado/input_driver"
DIR="Serious Sam 4"; [ "$1" = sm ] && DIR="Siberian Mayhem"
L="/home/obesecatlord/FasterGames/SteamLibrary/steamapps/common/$DIR/Bin/x64/ss4smvr/ss4smvr.log"
OUT=$2; N=${3:-6}
H="head=0,1.6,0,0,0,0"; LH="lpos=-0.2,1.3,-0.3"; R0="rpos=0.25,1.25,-0.15"
for i in $(seq 1 "$N"); do
  $ID $H $LH $R0 >/dev/null; sleep 1
  $ID $H $LH $R0 lgrip=1 >/dev/null; sleep 0.4; $ID $H $LH $R0 >/dev/null; sleep 3
  use=$(grep "gadgets: gadget in the" "$L" | tail -1 | tr -d '\r' | sed 's/.*hand: //')
  echo "[$i] $(grep "gadgets: gadget 0x" "$L" | tail -1 | sed 's/.*gadgets: //') -> $use"
  "$HERE/capture.sh" Monado "$OUT-g$i-held.png" 50 >/dev/null 2>&1
  case "$use" in
    "aimed throw"|toss)
      $ID $H $LH lrot=0,-0.35,0 $R0 >/dev/null; sleep 1.5  # aim at the ground ahead
      for z in 0.15 0.0 -0.15 -0.3 -0.45 -0.6 -0.75 -0.9; do $ID $H $LH lrot=0,-0.35,0 rpos=0.25,1.45,$z >/dev/null; done
      sleep 2.5
      if tail -5 "$L" | grep -q "without a valid target\|activation refused"; then  # too close: aim farther, throw again
        echo "[$i]   refused near target, aiming farther"
        $ID $H $LH lrot=0,0.05,0 $R0 >/dev/null; sleep 1.5
        for z in 0.15 0.0 -0.15 -0.3 -0.45 -0.6 -0.75 -0.9; do $ID $H $LH lrot=0,0.05,0 rpos=0.25,1.45,$z >/dev/null; done
      fi ;;
    "inject (arm or body)")
      if [ $((i % 2)) = 1 ]; then $ID $H $LH rpos=-0.17,1.3,-0.09 >/dev/null  # needle on the left forearm
      else $ID $H $LH rpos=0.15,1.0,0.05 rrot=1.5708,0,0 >/dev/null; fi; sleep 2 ;;  # needle into the right hip
    drink)
      $ID $H $LH rpos=0,1.51,-0.01 >/dev/null; sleep 2 ;;
    "press to the chest")
      $ID $H $LH rpos=0,1.28,-0.04 >/dev/null; sleep 2 ;;
    *)
      $ID $H $LH $R0 rtrig=1 >/dev/null; sleep 0.5 ;;
  esac
  sleep 0.5; "$HERE/capture.sh" Monado "$OUT-g$i-used.png" 50 >/dev/null 2>&1
  $ID $H $LH $R0 >/dev/null; sleep 3
  grep -E "gadgets: (.* -> use|holder use|launch|throw without)" "$L" | tail -5 | sed "s/^/[$i]   /"
  # next gadget: wheel held (left trackpad press), right A
  $ID $H $LH $R0 lforce=1 >/dev/null; sleep 0.5; $ID $H $LH $R0 lforce=1 ra=1 >/dev/null; sleep 0.3
  $ID $H $LH $R0 lforce=1 >/dev/null; sleep 0.3; $ID $H $LH $R0 >/dev/null; sleep 1.5
done
