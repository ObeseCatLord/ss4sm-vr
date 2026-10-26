#!/usr/bin/env python3
"""Give X input focus to the first window whose title contains <text> on the PRIVATE lab display.
With <cwd>, only a window whose process (_NET_WM_PID) runs in that directory, e.g. a lab tree's Bin/x64, so one of
several game instances with the same title can be chosen.
usage: xfocus.py <display> <title-substring> [<cwd>]"""
import os, sys
from Xlib import X, display

d = display.Display(sys.argv[1])
want_cwd = os.path.realpath(sys.argv[3]) if len(sys.argv) > 3 else None
PID = d.intern_atom('_NET_WM_PID')


def runs_in(w):
    if not want_cwd:
        return True
    p = w.get_full_property(PID, 0)
    try:
        return bool(p) and os.path.realpath(f'/proc/{p.value[0]}/cwd') == want_cwd
    except OSError:
        return False


def walk(w):
    try:
        n = w.get_wm_name()
        if isinstance(n, bytes): n = n.decode(errors='replace')
        if n and sys.argv[2] in n and w.get_geometry().width > 1 and runs_in(w): return w
        for c in w.query_tree().children:
            r = walk(c)
            if r: return r
    except Exception: pass
w = walk(d.screen().root)
if not w: sys.exit('window not found')
w.raise_window(); w.set_input_focus(X.RevertToParent, X.CurrentTime); d.sync()
print(hex(w.id))
