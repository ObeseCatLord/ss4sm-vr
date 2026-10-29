#!/bin/bash
# Lab: on-weapon ammo counters (docs/re/R32_ammo_counters.md): pistol (infinite ammo: none) and double shotgun, hand
# raised in front of the eyes, captures before and after three shots. usage: [OPTS="k=v"] ammo_counter_lab.sh [out-dir]
HERE="$(cd "$(dirname "$0")" && pwd)"; cd "$(dirname "$(dirname "$HERE")")"; T=tools/lab; . $T/paths.sh ss4; O=${1:-lab/runs/ammo5}; mkdir -p $O
ID=lab/monado/input_driver; H="head=0,1.6,0,0,0,0"; LH="lpos=-0.25,1.3,-0.35"; R0="rpos=0.25,1.3,-0.35"
M="$LAB_GAME/Bin/x64/ss4smvr"; printf 'lab_giveall=1\n%s\n' "$OPTS" > "$M/options.txt"; rm -f "$M/lab.lua"
$T/stop_lab.sh >/dev/null 2>&1
timeout 900 $T/level_test.sh ss4 Content/SeriousSam4/Levels/01_PB/02_Pompeii.wld $O/start >/dev/null 2>&1
$T/until_player.sh ss4 400 >/dev/null || { echo no player; exit 1; }
sleep 10
wheel_to() {
  $ID $H $LH $R0 rforce=1 >/dev/null; sleep 2
  $ID $H $LH rpos=$(awk "BEGIN{print 0.25+$1}"),$(awk "BEGIN{print 1.3+$2}"),-0.35 rforce=1 >/dev/null; sleep 1.2
  $ID $H $LH rpos=$(awk "BEGIN{print 0.25+$1}"),$(awk "BEGIN{print 1.3+$2}"),-0.35 >/dev/null; sleep 0.4
  $ID $H $LH $R0 >/dev/null; sleep 3
}
for w in "pistol -0.064 -0.064" "dshotgun 0.090 0"; do
  set -- $w; wheel_to $2 $3
  $ID $H $LH rpos=0.12,1.38,-0.32 >/dev/null; sleep 2
  $T/capture.sh Monado $O/$1-a.png 100
  # fire a few shots (count goes down)
  for k in 1 2 3; do $ID $H $LH rpos=0.12,1.38,-0.32 rtrig=1 >/dev/null; sleep 0.3; $ID $H $LH rpos=0.12,1.38,-0.32 >/dev/null; sleep 0.6; done
  sleep 1; $T/capture.sh Monado $O/$1-b.png 100
done
grep -a 'ammo:' "$MODLOG" | head -12
