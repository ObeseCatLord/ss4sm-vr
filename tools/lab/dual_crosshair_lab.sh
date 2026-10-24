#!/bin/bash
# Lab: one crosshair per weapon (R8 A7). Select a weapon per hand through the per-hand wheels (Index trackpad
# holds; the hand moves towards the item), point the hands apart and capture: one crosshair per gun, none for melee.
# usage: dual_crosshair_lab.sh <out-prefix> <right dir "x y"> <left dir "x y">
HERE="$(cd "$(dirname "$0")" && pwd)"; ROOT="$(dirname "$(dirname "$HERE")")"; ID="$ROOT/lab/monado/input_driver"
OUT=$1; RD=($2); LD=($3)
H="head=0,1.6,0,0,0,0"; R="0.25,1.3,-0.35"; L="-0.25,1.3,-0.35"
off() { awk "BEGIN{split(\"$1\",p,\",\"); printf \"%g,%g,%g\", p[1]+$2*0.09, p[2]+$3*0.09, p[3]}"; }
$ID $H lpos=$L rpos=$R rforce=1 >/dev/null; sleep 0.6; $ID $H lpos=$L rpos=$(off $R ${RD[0]} ${RD[1]}) rforce=1 >/dev/null; sleep 0.8
$ID $H lpos=$L rpos=$(off $R ${RD[0]} ${RD[1]}) >/dev/null; sleep 0.3; $ID $H lpos=$L rpos=$R >/dev/null; sleep 1.5
$ID $H lpos=$L rpos=$R lforce=1 >/dev/null; sleep 0.6; $ID $H lpos=$(off $L ${LD[0]} ${LD[1]}) rpos=$R lforce=1 >/dev/null; sleep 0.8
$ID $H lpos=$(off $L ${LD[0]} ${LD[1]}) rpos=$R >/dev/null; sleep 0.3; $ID $H lpos=$L rpos=$R >/dev/null; sleep 2
$ID $H lpos=-0.2,1.35,-0.45 lrot=0.25,0.08,0 rpos=0.2,1.35,-0.45 rrot=-0.2,-0.05,0 >/dev/null; sleep 2
"$HERE/capture.sh" Monado "$OUT.png" 100 >/dev/null 2>&1
