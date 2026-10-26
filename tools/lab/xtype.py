#!/usr/bin/env python3
"""Type text into the focused window of the PRIVATE lab display via XTEST (never the user's display).
usage: xtype.py <display> <text>   (shifted characters are typed with Shift held)"""
import sys, time
from Xlib import X, XK, display
from Xlib.ext import xtest

NAMES = {' ': 'space', '(': 'parenleft', ')': 'parenright', '"': 'quotedbl', ',': 'comma', '.': 'period', ':': 'colon',
         '/': 'slash', '|': 'bar', '_': 'underscore', '=': 'equal', '-': 'minus', ';': 'semicolon', "'": 'apostrophe'}
d = display.Display(sys.argv[1])
shift = d.keysym_to_keycode(XK.string_to_keysym('Shift_L'))
for ch in sys.argv[2]:
    sym = XK.string_to_keysym(NAMES.get(ch, ch))
    code = d.keysym_to_keycode(sym)
    shifted = d.keycode_to_keysym(code, 0) != sym
    if shifted: xtest.fake_input(d, X.KeyPress, shift)
    xtest.fake_input(d, X.KeyPress, code); xtest.fake_input(d, X.KeyRelease, code)
    if shifted: xtest.fake_input(d, X.KeyRelease, shift)
    d.sync(); time.sleep(0.02)
