#!/bin/sh
# Kill running LAB Sam4/SamSM game processes and their Proton launch wrappers (never shells). Only processes started
# from the lab copies (cwd or environment under lab/): a game the user runs from the Steam install is never touched.
exec 2>/dev/null
LAB="$(cd "$(dirname "$0")/../.." && pwd)/lab"
for d in /proc/[0-9]*; do
  a0=$(tr '\0' '\n' < "$d/cmdline" 2>/dev/null | head -1)
  a1=$(tr '\0' '\n' < "$d/cmdline" 2>/dev/null | sed -n 2p)
  a2=$(tr '\0' '\n' < "$d/cmdline" 2>/dev/null | sed -n 3p)
  hit=
  case "$a0" in
    *Sam4.exe|*Sam4_Unrestricted.exe|*SamSM.exe|*SamSM_Unrestricted.exe) hit=1;;
    *steam.exe) case "$a1" in *Sam4*.exe|*SamSM*.exe) hit=1;; esac;;
    python3) case "$a2" in run|waitforexitandrun) case "$(tr '\0' ' ' < "$d/cmdline")" in *Sam4*.exe*|*SamSM*.exe*) hit=1;; esac;; esac;;
  esac
  if [ -n "$hit" ]; then
    case "$(readlink "$d/cwd")" in "$LAB"/*) ;; *) tr '\0' '\n' < "$d/environ" | grep -q "^STEAM_COMPAT_DATA_PATH=$LAB/" || hit=;; esac
  fi
  [ -n "$hit" ] && { echo "kill ${d#/proc/} $a0 $a1"; kill -9 "${d#/proc/}"; }
done
# Lab wineservers only (their environment carries the lab's private XDG_RUNTIME_DIR): a server that outlives
# killed games can keep stale file handles (seen: the mod log could not be reopened).
sleep 1
for p in $(pgrep -x wineserver); do
  tr '\0' '\n' < /proc/$p/environ 2>/dev/null | grep -qx 'XDG_RUNTIME_DIR=/tmp/ss4vr-xr' && { echo "kill lab wineserver $p"; kill "$p"; }
done
exit 0
