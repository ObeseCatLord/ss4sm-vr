#!/bin/bash
# End of a lab session: stop the lab game and the lab Monado (only processes the lab started).
HERE="$(cd "$(dirname "$0")" && pwd)"
"$HERE/killgame.sh"
sleep 2
"$HERE/saves.sh" restore  # put the user's saves and profile back as they were before the lab run
PRIV=$HOME/ss2-vr-private/monado-exact-prefix/bin/monado-service
for p in $(pgrep -x monado-service); do
  [ "$(readlink /proc/$p/exe 2>/dev/null)" = "$PRIV" ] && { kill "$p"; echo "stopped lab monado $p"; }
done
rm -f "$HERE/../../lab/monado/monado.pid"
