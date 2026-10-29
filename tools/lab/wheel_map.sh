#!/bin/bash
# Lab: map the right hand's weapon wheel. For each of N directions, opens the wheel, moves the hand 9 cm that way,
# releases there and prints the weapon the right hand then holds (mod log "hands: right hand weapon").
# Before each probe the wheel selects a reference direction (RESET="dx dy", e.g. the pistol), so "(unchanged)"
# means the probe selected nothing new.
# WHEEL_R=<m> sets the hand offset (default 0.09: the inner ring; 0.28 reaches the outer ring, R18: inner R 0.15, outer R 0.30).
# usage: [RESET="dx dy"] wheel_map.sh ss4|sm [directions (default 16)]   (a level must be running, player ready)
HERE="$(cd "$(dirname "$0")" && pwd)"; ROOT="$(dirname "$(dirname "$HERE")")"; ID="$ROOT/lab/monado/input_driver"
. "$HERE/paths.sh" "$1"; N=${2:-16}
H="head=0,1.6,0,0,0,0"; LH="lpos=-0.25,1.3,-0.35"; R0="rpos=0.25,1.3,-0.35"
for i in $(seq 0 $((N - 1))); do
  a=$(awk "BEGIN{print 360*$i/$N}"); dx=$(awk "BEGIN{printf \"%.3f\", ${WHEEL_R:-0.09}*cos($a*3.14159265/180)}"); dy=$(awk "BEGIN{printf \"%.3f\", ${WHEEL_R:-0.09}*sin($a*3.14159265/180)}")
  if [ -n "$RESET" ]; then set -- $RESET
    $ID $H $LH $R0 rforce=1 >/dev/null; sleep 1.5
    $ID $H $LH rpos=$(awk "BEGIN{print 0.25+$1}"),$(awk "BEGIN{print 1.3+$2}"),-0.35 rforce=1 >/dev/null; sleep 1
    $ID $H $LH rpos=$(awk "BEGIN{print 0.25+$1}"),$(awk "BEGIN{print 1.3+$2}"),-0.35 >/dev/null; sleep 0.4
    $ID $H $LH $R0 >/dev/null; sleep 1.5
  fi
  start=$(wc -l < "$MODLOG")
  $ID $H $LH $R0 rforce=1 >/dev/null; sleep 2
  $ID $H $LH rpos=$(awk "BEGIN{print 0.25+$dx}"),$(awk "BEGIN{print 1.3+$dy}"),-0.35 rforce=1 >/dev/null; sleep 1.2
  $ID $H $LH rpos=$(awk "BEGIN{print 0.25+$dx}"),$(awk "BEGIN{print 1.3+$dy}"),-0.35 >/dev/null; sleep 0.4
  $ID $H $LH $R0 >/dev/null; sleep 2
  w=$(tail -n +"$start" "$MODLOG" | tr -d '\r' | grep -a "right hand weapon" | tail -1 | sed 's/.*right hand weapon //')
  printf '%5.1f deg  (%6s %6s)  %s\n' "$a" "$dx" "$dy" "${w:-(unchanged)}"
done
