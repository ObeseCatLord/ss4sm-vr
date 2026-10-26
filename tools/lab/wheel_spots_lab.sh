#!/bin/bash
# Lab: wheel spots (src/se4/wheel_panel.cpp). Opens the right hand's wheel, points at a spot (moves the hand past
# the wheel to the side), captures, releases there and prints the mod's spot log.
# usage: wheel_spots_lab.sh ss4|sm <out-prefix> <spot: menu|netricsa|load|save>
HERE="$(cd "$(dirname "$0")" && pwd)"; ROOT="$(dirname "$(dirname "$HERE")")"; ID="$ROOT/lab/monado/input_driver"
. "$HERE/paths.sh" "$1"; OUT=$2
case "$3" in menu) P=0.08,1.30;; netricsa) P=0.08,1.38;; load) P=0.42,1.30;; save) P=0.42,1.38;; *) echo "spot?"; exit 2;; esac
H="head=0,1.6,0,0,0,0"; LH="lpos=-0.25,1.3,-0.35"; R0="rpos=0.25,1.3,-0.35"
start=$(wc -l < "$MODLOG")
$ID $H $LH $R0 rforce=1 >/dev/null; sleep 2
$ID $H $LH rpos=$P,-0.35 rforce=1 >/dev/null; sleep 1.5
"$HERE/capture.sh" Monado "$OUT-$3-pointed.png" 100 >/dev/null 2>&1
$ID $H $LH rpos=$P,-0.35 >/dev/null; sleep 3
"$HERE/capture.sh" Monado "$OUT-$3-after.png" 100 >/dev/null 2>&1
$ID $H $LH $R0 >/dev/null
tail -n +"$start" "$MODLOG" | tr -d '\r' | grep -E "spot|wheel opened"
