#!/bin/bash
# End of a lab session: stop the lab game and the lab Monado (only processes the lab started).
HERE="$(cd "$(dirname "$0")" && pwd)"
"$HERE/killgame.sh"
PRIV=$HOME/ss2-vr-private/monado-exact-prefix/bin/monado-service
for p in $(pgrep -x monado-service); do
  [ "$(readlink /proc/$p/exe 2>/dev/null)" = "$PRIV" ] && { kill "$p"; echo "stopped lab monado $p"; }
done
rm -f "$HERE/../../lab/monado/monado.pid"
