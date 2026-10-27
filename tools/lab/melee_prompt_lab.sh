#!/bin/bash
# Lab: prompted melee by swing in VR (docs/re/R30_prompted_melee_swing.md). Enemies from the level's spawners are kept
# 0.9 m in front of the player (lab.lua); once the "Melee" prompt shows, the right hand swings through them and the
# log must show the swing kill, no canned melee start, and the prompt text without a button.
# usage: melee_prompt_lab.sh ss4|sm <out-dir>
HERE="$(cd "$(dirname "$0")" && pwd)"; ROOT="$(dirname "$(dirname "$HERE")")"
G=$1; OUT=$2; mkdir -p "$OUT"
. "$HERE/paths.sh" "$G"
LEVEL=${LEVEL:-Content/SeriousSam4/Levels/01_PB/02_Pompeii.wld}
[ "$G" = sm ] && LEVEL=Content/SeriousSamSM/Levels/01_SM/01_Refinery.wld
OPT="$LAB_GAME/Bin/x64/ss4smvr/options.txt"; LUA="$LAB_GAME/Bin/x64/ss4smvr/lab.lua"
ID=$ROOT/lab/monado/input_driver
H="head=0,1.6,0,0,0,0"; LH="lpos=-0.25,1.3,-0.35"; R0="rpos=0.25,1.3,-0.35"
cat > "$LUA" <<'LUAEOF'
-- lab only: enemies from the level's spawners for the prompted-melee swing test. Some spawners make decorative
-- (non-combat) actors, which are never offered for melee, so up to 8 are spawned and each in turn stands 0.9 m in
-- front of the player, facing it, for 6 s (stunned with a burst of damage, as a fight would); the rest wait 30 m away.
local wi = worldGlobals.worldInfo
local player = wi:GetPlayer()
local spawners = wi:GetAllEntitiesOfClass("CSpawnerEntity")
local foes, nxt, tries, cur, tick = {}, 1, 0, 1, 0
local STUN = false
local function spawn()
  for t = 1, #spawners do
    local s = spawners[nxt]
    nxt = nxt % #spawners + 1
    local ok, e = pcall(function() return s:SpawnSimpleNow() end)
    tries = tries + 1
    if ok and e and e:GetClassName() == "CLeggedCharacterEntity" and e:IsAlive() then
      -- only classes the game's melee selection accepts (the others are never prompted)
      local okSel, anim = pcall(function() return worldGlobals.PlayerMeleeSelection(e:GetCharacterClass(), "", 0.9, mthVector3f(0, 0, 0), 0, "Hands", 1, false, false) end)
      if okSel and anim then table.insert(foes, e); return end
      e:SetPlacement(mthQuatVect(e:GetPlacement():GetQuat(), e:GetPlacement():GetVect() + mthVector3f(0, -200, 0)))
    end
  end
end
RunAsync(function()
  while not IsDeleted(player) do
    player:SetHealth(200)
    local p = player:GetPlacement():GetVect()
    local d = player:GetLookDir(true)
    d = mthNormalize(mthVector3f(d.x, 0, d.z))
    if #foes < 8 and tries < 80 and #spawners > 0 then spawn() end
    local alive = {}
    for _, e in ipairs(foes) do if not IsDeleted(e) and e:IsAlive() then table.insert(alive, e) end end
    if #alive > 0 then
      tick = tick + 1
      if tick % 120 == 0 then cur = cur % #alive + 1 end
      for i, e in ipairs(alive) do
        local at = (i == (cur - 1) % #alive + 1) and 0.9 or 30
        e:SetPlacement(mthQuatVect(mthDirectionToQuaternion(d * -1), p + d * at))
        if at < 1 then
          if STUN and tick % 60 == 1 and not e:IsStunned() then pcall(function() e:InflictDamage(math.floor(e:GetHealth() * 0.3)) end) end
          if tick % 30 == 0 then
            conInfoF("lab_lua: foe " .. i .. "/" .. #alive .. " " .. e:GetCharacterClass() .. " health " .. e:GetHealth() .. " stunned " .. tostring(e:IsStunned()) .. "\n")
          end
        end
      end
    end
    Wait(Delay(0.05))
  end
end)
LUAEOF
printf '%s\n' lab_giveall=1 lab_lua_delay=1300 melee_prompt_debug=1 $EXTRA_OPTS > "$OPT"
"$HERE/stop_lab.sh" >/dev/null 2>&1
timeout 900 "$HERE/level_test.sh" "$G" "$LEVEL" "$OUT/start" >/dev/null 2>&1
"$HERE/until_player.sh" "$G" 400 >/dev/null || { echo "no player"; exit 1; }
sleep 10
# a gun in the right hand (the finder needs a hand weapon; a gun, not the knife, so its own blade cannot hit)
RIFLE="0.09 0"
wheel_to() {
  $ID $H $LH $R0 rforce=1 >/dev/null; sleep 2
  $ID $H $LH rpos=$(awk "BEGIN{print 0.25+$1}"),$(awk "BEGIN{print 1.3+$2}"),-0.35 rforce=1 >/dev/null; sleep 1.2
  $ID $H $LH rpos=$(awk "BEGIN{print 0.25+$1}"),$(awk "BEGIN{print 1.3+$2}"),-0.35 >/dev/null; sleep 0.4
  $ID $H $LH $R0 >/dev/null; sleep 2
}
wheel_to $RIFLE
sleep 5
"$HERE/capture.sh" Monado "$OUT/before.png" 50
kills=0
for i in $(seq 1 150); do
  # a fast right-hand swing across the front, through the enemy
  $ID $H $LH rpos=0.45,1.35,-0.15 >/dev/null; sleep 0.15
  $ID $H $LH rpos=-0.25,1.25,-0.75 >/dev/null; sleep 0.15
  [ $i = 2 ] && "$HERE/capture.sh" Monado "$OUT/swing.png" 50
  # the first swing kill: a few frames of the enemy's death; the prompt (no button glyph) while it is shown
  if [ -z "$deathcap" ] && grep -aq 'melee: prompted kill' "$MODLOG"; then
    deathcap=1
    for k in 1 2 3 4; do "$HERE/capture.sh" Monado "$OUT/death$k.png" 100; sleep 0.25; done
  fi
  if [ -z "$promptcap" ] && tail -1 "$MODLOG" | grep -aq 'prompt shown'; then promptcap=1; "$HERE/capture.sh" Monado "$OUT/prompt.png" 100; fi
done
rm -f "$LUA"
grep -aE 'melee: (prompt|prompted|Use/Melee)|melee:.*glyph|prompted melee' "$MODLOG" | tail -20
echo "kills: $(grep -ac 'melee: prompted kill' "$MODLOG")"
