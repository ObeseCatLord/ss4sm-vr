#!/bin/bash
# Start (or reuse) a private headless Weston + Xwayland display so lab runs never open windows on the
# user's desktop. Prints the X DISPLAY (e.g. :7). Stop with: private_display.sh stop
# Renderer gl composites on the GPU. Size 1920x1080@90.
LAB="$(cd "$(dirname "$0")/../.." && pwd)/lab/display"
RT=/tmp/ss4vr-rt          # short path: UNIX socket path length limit
mkdir -p "$LAB" "$RT"; chmod 700 "$RT"
if [ "$1" = stop ]; then
  [ -f "$LAB/weston.pid" ] && kill "$(cat "$LAB/weston.pid")" 2>/dev/null; rm -f "$LAB/weston.pid" "$LAB/display"; exit 0
fi
if [ -f "$LAB/weston.pid" ] && kill -0 "$(cat "$LAB/weston.pid")" 2>/dev/null && [ -s "$LAB/display" ]; then cat "$LAB/display"; exit 0; fi
rm -f "$LAB/weston.log" "$LAB/display"
env -u WAYLAND_DISPLAY -u DISPLAY XDG_RUNTIME_DIR=$RT setsid weston --backend=headless --renderer=gl --fake-seat \
  --refresh-rate=90000 --width=1920 --height=1080 --xwayland --no-config --idle-time=0 \
  --socket=ss4vr --log="$LAB/weston.log" < /dev/null > /dev/null 2>&1 &
echo $! > "$LAB/weston.pid"
for i in $(seq 1 50); do
  d=$(grep -oE 'xserver listening on display :[0-9]+' "$LAB/weston.log" 2>/dev/null | grep -oE ':[0-9]+' | tail -1)
  [ -n "$d" ] && { echo "$d" > "$LAB/display"; echo "$d"; exit 0; }
  sleep 0.2
done
echo "private display failed; see $LAB/weston.log" >&2; exit 1
