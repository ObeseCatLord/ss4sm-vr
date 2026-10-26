#!/bin/bash
# Lab test suite (isolated lab copy, simulated Monado): every VR feature checked from the mod's log, with captures.
# usage: suite.sh ss4|sm <out-dir> [stage ...]   stages: core vehicle survival melee artifact
# (default: core vehicle survival artifact on SS4; SM has no Survival map, so melee replaces survival)
# Prints PASS/FAIL per check and a summary; captures go to <out-dir>. Run tools/lab/stop_lab.sh afterwards.
# SUITE_OPTS="key=value ..." adds options to every stage (e.g. SUITE_OPTS=single_pass=1).
HERE="$(cd "$(dirname "$0")" && pwd)"; ROOT="$(dirname "$(dirname "$HERE")")"; ID="$ROOT/lab/monado/input_driver"
G=$1; OUT=$2; shift 2; mkdir -p "$OUT"
DEFAULT="core vehicle survival artifact"; [ "$G" = sm ] && DEFAULT="core vehicle melee artifact"
STAGES=${*:-$DEFAULT}
. "$HERE/paths.sh" "$G"
GAMELOG="$LAB_GAME/Log/$([ "$G" = sm ] && echo SamSM.log || echo Sam4.log)"
OPT="$LAB_GAME/Bin/x64/ss4smvr/options.txt"; LUA="$LAB_GAME/Bin/x64/ss4smvr/lab.lua"
if [ "$G" = sm ]; then
  CORE=Content/SeriousSamSM/Levels/01_SM/01_Refinery.wld; VEH=Content/SeriousSamSM/Levels/01_SM/02_Siberia.wld
  SURV=; INJ=Content/SeriousSamSM/Databases/Weapons/SirianInjector_SM.ep
  # right wheel directions (tools/lab/wheel_map.sh): AK 0 deg, crossbow (scoped) 120 deg, knife 240 deg
  RIFLE="0.09 0"; SCOPED="-0.045 0.078"; KNIFE="-0.045 -0.078"
  # vehicle: the Siberia snowmobile's handlebar grips (vehicles_log): half-width, height and reach from the head
  VX=0.32; VY=0.96; VZ=-0.57
else
  CORE=Content/SeriousSam4/Levels/01_PB/02_Pompeii.wld; VEH=Content/SeriousSam4/Levels/01_PB/06_Countryside.wld
  SURV=Content/SeriousSam4/Levels/Z5_Other/SV_Backyard.wld; INJ=Content/SeriousSam4/Databases/Weapons/SirianInjector.ep
  # right wheel directions (tools/lab/wheel_map.sh): rifle 0 deg, sniper 90 deg, knife 245 deg
  RIFLE="0.09 0"; SCOPED="0 0.09"; KNIFE="-0.038 -0.082"
  # vehicle: the Countryside harvester's wheel rim (18 cm fallback rim)
  VX=0.18; VY=1.13; VZ=-0.35
fi
H="head=0,1.6,0,0,0,0"; LH="lpos=-0.25,1.3,-0.35"; R0="rpos=0.25,1.3,-0.35"
PASS=0; FAIL=0; REPORT=
mark() { START=$(wc -l < "$MODLOG"); GSTART=$(wc -l < "$GAMELOG"); }
since() { tail -n +"$START" "$MODLOG" | tr -d '\r'; }
gsince() { tail -n +"$GSTART" "$GAMELOG" | tr -d '\r'; }
check() {  # check <name> <ok?> [detail]
  if [ "$2" = 1 ]; then PASS=$((PASS+1)); REPORT+="PASS  $1 $3"$'\n'; else FAIL=$((FAIL+1)); REPORT+="FAIL  $1 $3"$'\n'; fi
  echo "$( [ "$2" = 1 ] && echo PASS || echo FAIL )  $1 $3"
}
has() { since | grep -q -- "$1" && echo 1 || echo 0; }
ghas() { gsince | grep -q -- "$1" && echo 1 || echo 0; }
cap() { "$HERE/capture.sh" Monado "$OUT/$1.png" 100 >/dev/null 2>&1; }
press() { $ID $H $LH $R0 "$@" >/dev/null; sleep 0.4; $ID $H $LH $R0 >/dev/null; }
start() {  # start <level> [options lines...]
  local level=$1; shift
  printf '%s\n' "lab_giveall=1" $SUITE_OPTS "$@" > "$OPT"
  "$HERE/stop_lab.sh" >/dev/null 2>&1
  timeout 900 "$HERE/level_test.sh" "$G" "$level" "$OUT/start" +cht_bEnableCheats 1 +cht_bGod 1 "${EXTRA[@]}" >/dev/null 2>&1
  "$HERE/until_player.sh" "$G" 400 >/dev/null || { check "level $level starts" 0; return 1; }
  sleep 4; return 0
}
wheel_to() {  # wheel_to <dx> <dy> : open the right wheel, move the hand by (dx,dy) m, release there
  $ID $H $LH $R0 rforce=1 >/dev/null; sleep 2
  $ID $H $LH rpos=$(awk "BEGIN{print 0.25+$1}"),$(awk "BEGIN{print 1.3+$2}"),-0.35 rforce=1 >/dev/null; sleep 1.2
  $ID $H $LH rpos=$(awk "BEGIN{print 0.25+$1}"),$(awk "BEGIN{print 1.3+$2}"),-0.35 >/dev/null; sleep 0.4
  $ID $H $LH $R0 >/dev/null; sleep 2
}

stage_core() {
  rm -f "$LUA"; EXTRA=(); start "$CORE" || return
  mark
  check "installs: crosshair per weapon, spots, prompts, artifact, HUD inset, panel text" \
    "$(grep -aq 'one crosshair per weapon' "$MODLOG" && grep -aq 'spots beside the wheel' "$MODLOG" && grep -aq 'prompts: button prompts' "$MODLOG" && grep -aq 'artifact: Sirian artifact crush active' "$MODLOG" && grep -aq 'flat HUD inset' "$MODLOG" && grep -aq 'hand panel text drawn per eye' "$MODLOG" && echo 1 || echo 0)"
  check "no FAILED hooks" "$(grep -aq 'FAILED' "$MODLOG" && echo 0 || echo 1)" "$(grep -a 'FAILED' "$MODLOG" | head -2 | tr -d '\r')"
  cap core-view
  # Netricsa spot, tab switch with the grip, close with B
  mark; wheel_to -0.15 0.08; check "wheel spot: Netricsa" "$(has 'Netricsa spot run')"; cap netricsa
  press rgrip=1; sleep 1; press rb=1; sleep 2; cap netricsa-closed
  # Quick save / quick load (confirm YES with the pointer and trigger)
  mark; wheel_to 0.17 0.08; sleep 3; check "wheel spot: Quick save writes a save in the lab copy" "$(ghas 'Game successfully saved')"
  mark; wheel_to 0.17 0; sleep 2; cap quickload-dialog
  press ra=1  # menu layout: right A confirms the focused YES
  sleep 8; check "wheel spot: Quick load (native confirm, pointer YES)" "$(ghas 'Started loading save file')"
  sleep 12
  # Menu spot and the menu bar prompts (VR glyphs)
  mark; wheel_to -0.15 0; check "wheel spot: Menu" "$(has 'Menu spot run')"; cap menu
  check "menu bar asks the pad router for its shortcuts" "$(grep -aq 'prompts: button prompts show' "$MODLOG" && echo 1 || echo 0)"
  press rb=1; sleep 2
  # Rifle + left squeeze at the foregrip: two-hand grip (before dual wielding: the off hand must be free)
  mark; wheel_to $RIFLE; $ID $H lpos=0.25,1.3,-0.6 $R0 >/dev/null; sleep 1; $ID $H lpos=0.25,1.3,-0.6 $R0 lgrip=1 >/dev/null; sleep 2
  cap two-hand; $ID $H $LH $R0 >/dev/null
  check "two-hand grip (squeeze at the foregrip)" "$(has 'the two-handed weapon')"
  # Dual pistols: two crosshairs; knife right + pistol left: one
  "$HERE/dual_crosshair_lab.sh" "$OUT/dual-pistols" "-0.72 -0.70" "-0.72 -0.70" >/dev/null 2>&1
  check "dual pistols captured (two crosshairs: see dual-pistols.png)" 1
  mark; wheel_to $KNIFE; $ID $H lpos=-0.2,1.35,-0.45 lrot=0.25,0.08,0 rpos=0.2,1.35,-0.45 rrot=-0.2,-0.05,0 >/dev/null; sleep 2
  check "knife in the right hand" "$(has 'melee: knife in hand')"; cap knife-left-pistol
  # Scoped weapon (SS4 sniper, SM crossbow), raised to the eye
  mark; wheel_to $SCOPED; $ID $H $LH rpos=0.04,1.55,-0.12 >/dev/null; sleep 3; cap scope
  check "scoped weapon: picture-in-picture scope" "$(has 'scope: scoped weapon')"
  # Gadgets (each gesture), from the left grip
  mark; "$HERE/gadgets_lab.sh" "$G" "$OUT/gadget" 6 >/dev/null 2>&1
  check "gadgets: taken into the hand and used" "$(since | grep -c -- '-> use' | awk '{print ($1>=3)?1:0}')" "($(since | grep -c -- '-> use') uses)"
}

stage_vehicle() {
  cat > "$LUA" <<'EOF'
-- lab only: board the first vehicle (vehicle tests)
local wi = worldGlobals.worldInfo
local player = wi:GetPlayer()
local v = wi:GetAllEntitiesOfClass("CVehicleEntity")[1]
local qv = v:GetPlacement(); qv:SetVect(qv:GetVect() + mthVector3f(0, 0.5, 2.5))
player:TeleportQV(qv, false)
player:MountRide(v, v:GetSeatName(0))
conInfoF("lab_lua: mounted\n")
EOF
  EXTRA=(); start "$VEH" "lab_lua_delay=60" "vehicles_log=1" || return
  sleep 10; mark
  check "vehicle: boarded without a crash" "$(grep -aq 'vehicles: driving' "$MODLOG" && ! grep -aq Crashed "$GAMELOG" && echo 1 || echo 0)"
  check "vehicle: seated height" "$(grep -aq 'seated height' "$MODLOG" && echo 1 || echo 0)"
  check "vehicle: riding view first person (head camera at the seat)" "$(grep -aq 'riding view kept first person' "$MODLOG" && echo 1 || echo 0)"
  cap vehicle-seat
  $ID $H lpos=-$VX,$VY,$VZ rpos=$VX,$VY,$VZ >/dev/null; sleep 1
  $ID $H lpos=-$VX,$VY,$VZ rpos=$VX,$VY,$VZ lgrip=1 rgrip=1 >/dev/null; sleep 2
  for a in 0.15 0.3 0.45 0.6; do  # both hands turn about the steering axis
    $ID $H lpos=$(awk "BEGIN{print -$VX*cos($a)}"),$VY,$(awk "BEGIN{print $VZ+$VX*sin($a)}") \
      rpos=$(awk "BEGIN{print $VX*cos($a)}"),$VY,$(awk "BEGIN{print $VZ-$VX*sin($a)}") lgrip=1 rgrip=1 >/dev/null; sleep 0.7
  done
  check "vehicle: steering wheel grabbed" "$(grep -aq 'grabbed the steering' "$MODLOG" && echo 1 || echo 0)"
  $ID $H $LH $R0 lstick=0,1 >/dev/null; sleep 4; cap vehicle-driving; $ID $H $LH $R0 >/dev/null; sleep 1
  press rb=1; sleep 3; cap vehicle-out
  check "vehicle: still running after driving and getting out" "$(grep -aq Crashed "$GAMELOG" && echo 0 || echo 1)"
  rm -f "$LUA"
}

stage_survival() {
  [ -n "$SURV" ] || return
  cat > "$LUA" <<'EOF'
-- lab only: keep the local player alive (Survival kick/melee tests)
local player = worldGlobals.worldInfo:GetPlayer()
RunAsync(function()
  while not IsDeleted(player) do player:SetHealth(200); Wait(Delay(0.25)) end
end)
EOF
  EXTRA=(+gam_idGameMode Survival); start "$SURV" "lab_lua_delay=30" || return
  sleep 10; mark
  for k in $(seq 1 14); do press rb=1; sleep 0.5; done
  check "kick: right B runs the native use/melee" "$(has 'right B - native use')"
  sleep 1; mark; wheel_to $KNIFE
  for k in 1 2 3 4 5 6; do
    for x in 0.45 0.3 0.15 0.0 -0.15 -0.3; do $ID $H $LH rpos=$x,1.35,-0.5 >/dev/null; done; sleep 0.3
    for x in -0.3 -0.15 0.0 0.15 0.3 0.45; do $ID $H $LH rpos=$x,1.25,-0.5 >/dev/null; done; sleep 0.3
  done
  check "melee: knife swings hit enemies" "$(has 'melee: knife hit')" "($(since | grep -c 'melee: knife hit') hits)"
  cap survival
  rm -f "$LUA"
}

stage_melee() {  # kick and knife hits on the core level, with enemies from its spawners (games without Survival)
  cat > "$LUA" <<'EOF'
-- lab only: enemies for the melee tests. Whenever fewer than two are alive, the level's spawners spawn one more
-- (SpawnSimpleNow returns the spawnee); the living ones are kept 1.4-1.9 m in front of the player.
local wi = worldGlobals.worldInfo
local player = wi:GetPlayer()
local spawners = wi:GetAllEntitiesOfClass("CSpawnerEntity")
local foes, next, total, shown = {}, 1, 0, false
local function spawn(p, d)
  for tries = 1, #spawners do
    local s = spawners[next]
    next = next % #spawners + 1
    -- spawned where the level placed the spawner; only characters are used (some spawners make vehicles or items)
    local ok, e = pcall(function() return s:SpawnSimpleNow() end)
    total = total + 1
    if ok and e and e:GetClassName() == "CLeggedCharacterEntity" and e:IsAlive() then table.insert(foes, e); return end
  end
end
RunAsync(function()
  while not IsDeleted(player) do
    player:SetHealth(200)
    local p = player:GetPlacement():GetVect()
    local d = player:GetLookDir(true)
    local k = 0
    for _, e in ipairs(foes) do
      if k < 2 and not IsDeleted(e) and e:IsAlive() then
        local qv = e:GetPlacement()
        qv:SetVect(p + d * (1.4 + k * 0.5))
        e:SetPlacement(qv)
        k = k + 1
      end
    end
    if k < 2 and total < 60 and #spawners > 0 then spawn(p, d) end
    if k > 0 and not shown then conInfoF("lab_lua: enemies in front of the player\n"); shown = true end
    Wait(Delay(0.5))
  end
end)
EOF
  EXTRA=(); start "$CORE" "lab_lua_delay=60" || return
  # The knife first: the kick presses "use", which can also board a vehicle the spawners left nearby (SM Refinery boats).
  sleep 10; mark; wheel_to $KNIFE
  for k in 1 2 3 4 5 6; do
    for x in 0.45 0.3 0.15 0.0 -0.15 -0.3; do $ID $H $LH rpos=$x,1.35,-0.5 >/dev/null; done; sleep 0.3
    for x in -0.3 -0.15 0.0 0.15 0.3 0.45; do $ID $H $LH rpos=$x,1.25,-0.5 >/dev/null; done; sleep 0.3
  done
  check "melee: knife swings hit enemies" "$(has 'melee: knife hit')" "($(since | grep -c 'melee: knife hit') hits)"
  cap melee
  mark
  for k in $(seq 1 4); do press rb=1; sleep 0.5; done
  check "kick: right B runs the native use/melee" "$(has 'right B - native use')"
  rm -f "$LUA"
}

stage_artifact() {
  cat > "$LUA" <<EOF
-- lab only: start the Sirian artifact "Use" animation on the local player (R17/R20)
local wi = worldGlobals.worldInfo
local player = wi:GetPlayer()
local pwp = wi:LoadResource(Depfile("$INJ"))
prjStartGenericWeaponAnimation(player, pwp, "Use", "GWASF_STOP_MOVING|GWASF_STOP_LOOKING|GWASF_GIVE_NEW_WEAPON")
EOF
  EXTRA=(); start "$CORE" "lab_lua_delay=90" || return
  sleep 6; mark; sleep 4
  check "artifact: held in the hand" "$(grep -aq 'artifact: held at anim' "$MODLOG" && echo 1 || echo 0)"
  cap artifact-held
  $ID $H $LH rpos=0.2,1.35,-0.45 rtrig=1 >/dev/null; sleep 0.5; $ID $H $LH $R0 >/dev/null; sleep 4
  check "artifact: crushed, ArtifactBroken fired" "$(grep -aq 'released (crushed)' "$MODLOG" && grep -aq 'global script anim event fired' "$MODLOG" && echo 1 || echo 0)"
  rm -f "$LUA"
}

for s in $STAGES; do "stage_$s"; done
"$HERE/stop_lab.sh" >/dev/null 2>&1
printf '%s' "$REPORT" > "$OUT/report.txt"
echo "== $G: $PASS passed, $FAIL failed (report $OUT/report.txt)"
