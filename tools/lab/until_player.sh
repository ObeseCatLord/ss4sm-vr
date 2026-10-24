#!/bin/bash
# Lab: press A on the simulated right controller until the local CPlayerVRData exists (skips intro cutscenes).
# usage: until_player.sh ss4|sm [timeout-seconds]
HERE="$(cd "$(dirname "$0")" && pwd)"; ROOT="$(dirname "$(dirname "$HERE")")"
DIR="Serious Sam 4"; [ "$1" = sm ] && DIR="Siberian Mayhem"
L="/home/obesecatlord/FasterGames/SteamLibrary/steamapps/common/$DIR/Bin/x64/ss4smvr/ss4smvr.log"
END=$((SECONDS + ${2:-300}))
while [ $SECONDS -lt $END ]; do
  grep -q "(local 1)" "$L" && { echo "player ready after ${SECONDS}s"; exit 0; }
  "$ROOT/lab/monado/input_driver" ra=1 >/dev/null; sleep 1.5; "$ROOT/lab/monado/input_driver" ra=0 >/dev/null; sleep 5
done
echo "no local player"; exit 1
