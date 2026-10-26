#!/bin/bash
# Keep lab runs out of the user's saves and profile. The games write auto/checkpoint saves as rotating QuickSaves and
# the player profile into Steam's per-user cloud folder (userdata/<id>/<app>/remote) on every level start; the lab
# cannot avoid running those code paths. So:
#   saves.sh snapshot   before a lab run: copy each game's remote folder to lab/saves-snapshot (kept if one exists,
#                       so the snapshot is always the state from before the first unrestored run)
#   saves.sh restore    after the run (stop_lab.sh): make each remote folder identical to its snapshot again
#                       (lab-made files removed, changed/rotated files put back), then drop the snapshot.
HERE="$(cd "$(dirname "$0")" && pwd)"; SNAP="$HERE/../../lab/saves-snapshot"
apps="257420 1792250"
case "$1" in
  snapshot)
    [ -f "$SNAP/.complete" ] && { echo "saves: snapshot from $(cat "$SNAP/.complete") kept"; exit 0; }
    rm -rf "$SNAP"; mkdir -p "$SNAP"
    for app in $apps; do
      for d in "$HOME"/.local/share/Steam/userdata/*/"$app"/remote; do
        [ -d "$d" ] || continue
        id=$(basename "$(dirname "$(dirname "$d")")")
        mkdir -p "$SNAP/$id/$app" && rsync -a "$d/" "$SNAP/$id/$app/remote/" || { echo "saves: snapshot of $d FAILED"; exit 1; }
      done
    done
    date -Is > "$SNAP/.complete"; echo "saves: snapshot taken";;
  restore)
    echo "saves: automatic restore disabled (it desynchronised Steam's cloud cache, 2026-10-25) - nothing changed"; exit 1
    [ -f "$SNAP/.complete" ] || { echo "saves: no snapshot"; exit 0; }
    for s in "$SNAP"/*/*/remote; do
      app=$(basename "$(dirname "$s")"); id=$(basename "$(dirname "$(dirname "$s")")")
      d="$HOME/.local/share/Steam/userdata/$id/$app/remote"
      [ -d "$d" ] || continue
      changes=$(rsync -a --delete --checksum --itemize-changes "$s/" "$d/" | wc -l)
      echo "saves: $id/$app restored ($changes changes undone)"
    done
    rm -rf "$SNAP";;
  *) echo "usage: $0 snapshot|restore"; exit 2;;
esac
