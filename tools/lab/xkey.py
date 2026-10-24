#!/usr/bin/env python3
"""Send key presses to the PRIVATE lab display via XTEST (never the user's display).
usage: xkey.py <display> <keysym> [<keysym> ...]   e.g. xkey.py :3 Return space"""
import sys, time
from Xlib import X, XK, display
from Xlib.ext import xtest

d = display.Display(sys.argv[1])
for name in sys.argv[2:]:
    code = d.keysym_to_keycode(XK.string_to_keysym(name))
    xtest.fake_input(d, X.KeyPress, code); d.sync(); time.sleep(0.15)
    xtest.fake_input(d, X.KeyRelease, code); d.sync(); time.sleep(0.3)
