#!/bin/bash
# (Re)start the PRIVATE simulated Monado (remote driver, port 42431) on the private display.
# Only ever stops the process recorded in our own pid file, and only if it is the private build.
# Never touches the user's Envision/Monado. Refuses to start while another Monado owns the IPC socket.
LAB="$(cd "$(dirname "$0")/../.." && pwd)/lab"
PRIV=$HOME/ss2-vr-private/monado-exact-prefix/bin/monado-service
PIDF="$LAB/monado/monado.pid"
# Private XDG_RUNTIME_DIR: the lab runtime's IPC socket (monado_comp_ipc) and pid file live here, never in
# /run/user/<uid>, so it can never take the user's Monado socket (2026-10-23 incident). Games launched by
# run_game.sh use the same directory.
XRT_RT=/tmp/ss4vr-xr; mkdir -p "$XRT_RT"; chmod 700 "$XRT_RT"
if [ -f "$PIDF" ]; then
  p=$(cat "$PIDF")
  if [ "$(readlink /proc/$p/exe 2>/dev/null)" = "$PRIV" ]; then kill "$p"; sleep 1; fi
  rm -f "$PIDF"
fi
for p in $(pgrep -x monado-service); do
  if [ "$(readlink /proc/$p/exe 2>/dev/null)" != "$PRIV" ]; then
    echo "another Monado is running (pid $p: $(readlink /proc/$p/exe)) - not starting the lab runtime" >&2; exit 2
  fi
done
D=$("$(dirname "$0")/private_display.sh") || exit 1
cd "$LAB/monado"
( tail -f /dev/null | env -u WAYLAND_DISPLAY DISPLAY=$D XDG_RUNTIME_DIR=$XRT_RT XDG_CONFIG_HOME=$LAB/monado/config P_OVERRIDE_ACTIVE_CONFIG=remote \
  XRT_COMPOSITOR_FORCE_XCB=1 XRT_COMPOSITOR_COMPUTE=0 U_PACING_APP_USE_MIN_FRAME_PERIOD=1 XRT_DEBUG_GUI=0 XRT_CURATED_GUI=0 \
  setsid "$PRIV" > "$LAB/monado/monado.log" 2>&1 & )
for i in $(seq 1 40); do
  for p in $(pgrep -x monado-service); do
    [ "$(readlink /proc/$p/exe)" = "$PRIV" ] && [ -S "$XRT_RT/monado_comp_ipc" ] && { echo $p > "$PIDF"; echo "lab monado $p up on $D"; exit 0; }
  done
  sleep 0.5
done
echo "monado failed" >&2; exit 1
