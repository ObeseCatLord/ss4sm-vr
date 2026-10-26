#!/bin/bash
# Lab: performance matrix (docs/PERF.md). For each options line, starts the scene in the isolated lab copy, lets it
# settle, and reports the median of the mod's 5 s "perf:" windows (GPU ms per frame summed over the XR queue's
# submissions, game CPU ms per frame without the XR wait) and the eye size. The lab compositor paces at 60 Hz, so
# frame rate is not the measure; GPU and CPU time per frame are.
# usage: perf_matrix.sh ss4|sm <scene: pompeii|survival|refinery> <out-dir> "<options line>" ...
#   e.g. perf_matrix.sh ss4 survival lab/runs/perf "render_scale=170" "render_scale=170 foveation=1"
HERE="$(cd "$(dirname "$0")" && pwd)"; ROOT="$(dirname "$(dirname "$HERE")")"; ID="$ROOT/lab/monado/input_driver"
G=$1; SCENE=$2; OUT=$3; shift 3; mkdir -p "$OUT"
. "$HERE/paths.sh" "$G"
OPT="$LAB_GAME/Bin/x64/ss4smvr/options.txt"; LUA="$LAB_GAME/Bin/x64/ss4smvr/lab.lua"
EXTRA=(); LUAOPT=
case "$SCENE" in
  pompeii) LEVEL=Content/SeriousSam4/Levels/01_PB/02_Pompeii.wld;;
  refinery) LEVEL=Content/SeriousSamSM/Levels/01_SM/01_Refinery.wld;;
  survival) LEVEL=Content/SeriousSam4/Levels/Z5_Other/SV_Backyard.wld; EXTRA=(+gam_idGameMode Survival); LUAOPT=lab_lua_delay=30
    cat > "$LUA" <<'EOF'
-- lab only: keep the local player alive (Survival performance scene)
local player = worldGlobals.worldInfo:GetPlayer()
RunAsync(function()
  while not IsDeleted(player) do player:SetHealth(200); Wait(Delay(0.25)) end
end)
EOF
    ;;
  *) echo "scene?"; exit 2;;
esac
[ "$SCENE" = survival ] || rm -f "$LUA"
printf '%-44s %8s %8s %8s %8s  %s\n' "options" "GPU ms" "CPU ms" "CPU max" "subm/f" "eye size"
for cfg in "$@"; do
  tag=$(echo "$cfg" | tr ' =' '_-'); [ -z "$tag" ] && tag=default
  { echo lab_giveall=1; [ -n "$LUAOPT" ] && echo "$LUAOPT"; for kv in $cfg; do echo "$kv"; done; } > "$OPT"
  "$HERE/stop_lab.sh" >/dev/null 2>&1
  timeout 900 "$HERE/level_test.sh" "$G" "$LEVEL" "$OUT/$tag" +cht_bEnableCheats 1 +cht_bGod 1 "${EXTRA[@]}" >/dev/null 2>&1
  if ! "$HERE/until_player.sh" "$G" 400 >/dev/null; then printf '%-44s  no player\n' "$cfg"; continue; fi
  "$ID" head=0,1.6,0,0,0,0 lpos=-0.25,1.3,-0.35 rpos=0.25,1.3,-0.35 >/dev/null
  sleep 40; start=$(wc -l < "$MODLOG"); sleep 60
  "$HERE/capture.sh" Monado "$OUT/$tag-scene.png" 50 >/dev/null 2>&1
  eye=$(grep -a "xr: eye size" "$MODLOG" | tail -1 | sed 's/.*eye size \([0-9x]*\).*/\1/' | tr -d '\r')
  tail -n +"$start" "$MODLOG" | tr -d '\r' | grep -a "perf: GPU" | python3 -I -c "
import sys, re, statistics as st
rows = [re.findall(r'[\d.]+', l.split('perf: ')[1]) for l in sys.stdin]
rows = [(float(r[0]), float(r[1]), float(r[2]), float(r[3])) for r in rows if len(r) >= 4]
if not rows: print('%-44s  no perf lines' % sys.argv[1]); sys.exit()
g = st.median(r[0] for r in rows); s = st.median(r[1] for r in rows); c = st.median(r[2] for r in rows); m = max(r[3] for r in rows)
print('%-44s %8.2f %8.2f %8.2f %8.1f  %s' % (sys.argv[1], g, c, m, s, sys.argv[2]))
" "${cfg:-default}" "$eye"
  grep -aq "Crashed" "$LAB_GAME/Log/$([ "$G" = sm ] && echo SamSM.log || echo Sam4.log)" && echo "  (game log reports a crash)"
done
rm -f "$LUA"
"$HERE/stop_lab.sh" >/dev/null 2>&1
