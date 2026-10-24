#!/usr/bin/env python3
"""Move the pointer into a window on the PRIVATE lab display and click it (XTEST), so Xwayland gives it
keyboard focus. usage: xclick.py <display> <title-substring> [x y] (window-relative, default centre)"""
import sys, time
from Xlib import X, display
from Xlib.ext import xtest

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
g = w.get_geometry()
t = w.translate_coords(d.screen().root, 0, 0)
x0, y0 = -t.x, -t.y
x = x0 + (int(sys.argv[3]) if len(sys.argv) > 4 else g.width // 2)
y = y0 + (int(sys.argv[4]) if len(sys.argv) > 4 else g.height // 2)
w.raise_window(); d.sync()
xtest.fake_input(d, X.MotionNotify, x=x, y=y); d.sync(); time.sleep(0.2)
xtest.fake_input(d, X.ButtonPress, 1); d.sync(); time.sleep(0.1)
xtest.fake_input(d, X.ButtonRelease, 1); d.sync()
print(hex(w.id), x, y)
