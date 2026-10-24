#!/usr/bin/env python3
"""Give X input focus to the first window whose title contains <text> on the PRIVATE lab display.
usage: xfocus.py <display> <title-substring>"""
import sys
from Xlib import X, display

d = display.Display(sys.argv[1])
def walk(w):
    try:
        n = w.get_wm_name()
        if isinstance(n, bytes): n = n.decode(errors='replace')
        if n and sys.argv[2] in n and w.get_geometry().width > 1: return w
        for c in w.query_tree().children:
            r = walk(c)
            if r: return r
    except Exception: pass
w = walk(d.screen().root)
if not w: sys.exit('window not found')
w.raise_window(); w.set_input_focus(X.RevertToParent, X.CurrentTime); d.sync()
print(hex(w.id))
