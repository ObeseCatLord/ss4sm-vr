#!/bin/bash
# Lab: in a running level with the local player, step through the weapons (Index left trackpad press tap =
# previous weapon) and capture the headset view after each, with the hands raised in front.
# usage: weapons_tour.sh <out-prefix> [count]
HERE="$(cd "$(dirname "$0")" && pwd)"; ROOT="$(dirname "$(dirname "$HERE")")"; ID="$ROOT/lab/monado/input_driver"
OUT=$1; N=${2:-8}
POSE="head=0,1.6,0,0,0,0 lpos=-0.15,1.35,-0.45 rpos=0.15,1.35,-0.35"
for i in $(seq 1 "$N"); do
  $ID $POSE lforce=1 >/dev/null; sleep 0.3; $ID $POSE >/dev/null; sleep 2.5
  "$HERE/capture.sh" Monado "$OUT-w$i.png" 50 >/dev/null 2>&1
done
# squeeze the left hand near the right (grip attempt), then release
$ID $POSE lgrip=1 >/dev/null; sleep 1.5; "$HERE/capture.sh" Monado "$OUT-grip.png" 50 >/dev/null 2>&1
$ID $POSE >/dev/null
