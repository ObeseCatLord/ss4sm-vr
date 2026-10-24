#!/bin/bash
# Capture a window from the private display by title substring. usage: capture.sh <title> <out.png> [resize%]
D=$("$(dirname "$0")/private_display.sh") || exit 1
W=$(DISPLAY=$D python3 -I - "$1" <<'PY'
import sys
from Xlib import display
d = display.Display(); want = sys.argv[1]
def walk(w):
    try:
        name = w.get_wm_name()
        if isinstance(name, bytes): name = name.decode(errors='replace')
        if name and want in name:
            g = w.get_geometry()
            if g.width > 1 and g.height > 1: print(hex(w.id)); sys.exit(0)
        for c in w.query_tree().children: walk(c)
    except SystemExit: raise
    except Exception: pass
walk(d.screen().root)
PY
)
[ -z "$W" ] && { echo "no window '$1' on $D" >&2; exit 1; }
DISPLAY=$D timeout 10 import -window "$W" ${3:+-resize $3%} "$2"
