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
    # Prefer a main-keyboard key (keycode < 100, level 0 or Shift): some keysyms, e.g. parentheses, are also on extra
    # keys (keycode 187/188) that Wine does not translate.
    codes = sorted((c, i) for c, i in d.keysym_to_keycodes(sym) if i in (0, 1))
    code = next((c for c, i in codes if c < 100), codes[0][0] if codes else d.keysym_to_keycode(sym))
    shifted = d.keycode_to_keysym(code, 0) != sym
    if shifted: xtest.fake_input(d, X.KeyPress, shift)
    xtest.fake_input(d, X.KeyPress, code); xtest.fake_input(d, X.KeyRelease, code)
    if shifted: xtest.fake_input(d, X.KeyRelease, shift)
    d.sync(); time.sleep(0.02)
