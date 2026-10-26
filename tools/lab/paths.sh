# Sourced by lab scripts: LAB_GAME = the isolated lab copy of the game (tools/lab/make_lab_tree.sh) for $1 (ss4|sm)
# and LAB_INSTANCE (default 1). Logs: $LAB_GAME/Log/<exe>.log, mod log $LAB_GAME/Bin/x64/ss4smvr/ss4smvr.log.
_k=ss4; [ "$1" = sm ] && _k=sm
_n=$_k; [ "${LAB_INSTANCE:-1}" != 1 ] && _n="$_k-$LAB_INSTANCE"
LAB_GAME="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)/lab/game/$_n"
MODLOG="$LAB_GAME/Bin/x64/ss4smvr/ss4smvr.log"
