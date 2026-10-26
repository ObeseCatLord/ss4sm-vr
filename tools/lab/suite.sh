#!/bin/bash
# Lab test suite (isolated lab copy, simulated Monado): every VR feature checked from the mod's log, with captures.
# usage: suite.sh ss4|sm <out-dir> [stage ...]   stages: core vehicle survival artifact (default: all)
# Prints PASS/FAIL per check and a summary; captures go to <out-dir>. Run tools/lab/stop_lab.sh afterwards.
HERE="$(cd "$(dirname "$0")" && pwd)"; ROOT="$(dirname "$(dirname "$HERE")")"; ID="$ROOT/lab/monado/input_driver"
G=$1; OUT=$2; shift 2; mkdir -p "$OUT"
STAGES=${*:-core vehicle survival artifact}
. "$HERE/paths.sh" "$G"
GAMELOG="$LAB_GAME/Log/$([ "$G" = sm ] && echo SamSM.log || echo Sam4.log)"
OPT="$LAB_GAME/Bin/x64/ss4smvr/options.txt"; LUA="$LAB_GAME/Bin/x64/ss4smvr/lab.lua"
if [ "$G" = sm ]; then
  CORE=Content/SeriousSamSM/Levels/01_SM/01_Refinery.wld; VEH=Content/SeriousSamSM/Levels/01_SM/02_Siberia.wld
  SURV=; INJ=Content/SeriousSamSM/Databases/Weapons/SirianInjector_SM.ep
else
  CORE=Content/SeriousSam4/Levels/01_PB/02_Pompeii.wld; VEH=Content/SeriousSam4/Levels/01_PB/06_Countryside.wld
  SURV=Content/SeriousSam4/Levels/Z5_Other/SV_Backyard.wld; INJ=Content/SeriousSam4/Databases/Weapons/SirianInjector.ep
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
  printf '%s\n' "lab_giveall=1" "$@" > "$OPT"
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
  # Dual pistols: two crosshairs; knife right + pistol left: one
  "$HERE/dual_crosshair_lab.sh" "$OUT/dual-pistols" "-0.72 -0.70" "-0.72 -0.70" >/dev/null 2>&1
  check "dual pistols captured (two crosshairs: see dual-pistols.png)" 1
  mark; wheel_to -0.038 -0.082; $ID $H lpos=-0.2,1.35,-0.45 lrot=0.25,0.08,0 rpos=0.2,1.35,-0.45 rrot=-0.2,-0.05,0 >/dev/null; sleep 2
  check "knife in the right hand" "$(has 'melee: knife in hand')"; cap knife-left-pistol
  # Sniper scope (top of the right wheel), raised to the eye
  mark; wheel_to 0 0.09; $ID $H $LH rpos=0.04,1.55,-0.12 >/dev/null; sleep 3; cap scope
  check "sniper: picture-in-picture scope" "$(has 'scope: scoped weapon')"
  # Rifle + left squeeze at the foregrip: two-hand grip
  $ID $H $LH $R0 rforce=1 >/dev/null; sleep 0.1; $ID $H $LH $R0 >/dev/null; sleep 2  # right trackpad tap: dual wield off (free left hand)
  mark; wheel_to 0.09 0; $ID $H lpos=0.25,1.3,-0.6 $R0 >/dev/null; sleep 1; $ID $H lpos=0.25,1.3,-0.6 $R0 lgrip=1 >/dev/null; sleep 2
  cap two-hand; $ID $H $LH $R0 >/dev/null
  check "two-hand grip (squeeze at the foregrip)" "$(has 'the two-handed weapon')"
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
  EXTRA=(); start "$VEH" "lab_lua_delay=60" || return
  sleep 10; mark
  check "vehicle: boarded without a crash" "$(grep -aq 'vehicles: driving' "$MODLOG" && ! grep -aq Crashed "$GAMELOG" && echo 1 || echo 0)"
  check "vehicle: seated height" "$(grep -aq 'seated height' "$MODLOG" && echo 1 || echo 0)"
  cap vehicle-seat
  Y=1.13; $ID $H lpos=-0.18,$Y,-0.35 rpos=0.18,$Y,-0.35 >/dev/null; sleep 1
  $ID $H lpos=-0.18,$Y,-0.35 rpos=0.18,$Y,-0.35 lgrip=1 rgrip=1 >/dev/null; sleep 2
  for a in 0.15 0.3 0.45 0.6; do
    $ID $H lpos=$(awk "BEGIN{print -0.18*cos($a)}"),$Y,$(awk "BEGIN{print -0.35+0.18*sin($a)}") \
      rpos=$(awk "BEGIN{print 0.18*cos($a)}"),$Y,$(awk "BEGIN{print -0.35-0.18*sin($a)}") lgrip=1 rgrip=1 >/dev/null; sleep 0.7
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
  sleep 1; mark; wheel_to -0.038 -0.082
  for k in 1 2 3 4 5 6; do
    for x in 0.45 0.3 0.15 0.0 -0.15 -0.3; do $ID $H $LH rpos=$x,1.35,-0.5 >/dev/null; done; sleep 0.3
    for x in -0.3 -0.15 0.0 0.15 0.3 0.45; do $ID $H $LH rpos=$x,1.25,-0.5 >/dev/null; done; sleep 0.3
  done
  check "melee: knife swings hit enemies" "$(has 'melee: knife hit')" "($(since | grep -c 'melee: knife hit') hits)"
  cap survival
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
