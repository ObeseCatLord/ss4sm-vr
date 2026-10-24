#!/bin/sh
# Kill running Sam4/SamSM game processes and their Proton launch wrappers (never shells).
exec 2>/dev/null
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
  [ -n "$hit" ] && { echo "kill ${d#/proc/} $a0 $a1"; kill -9 "${d#/proc/}"; }
done
exit 0
