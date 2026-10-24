#!/bin/bash
# Lab: select the SS4 sniper rifle from the native weapon wheel (all weapons given; Index left trackpad press holds
# the wheel, the right stick points at the item: up and slightly left in the default layout).
HERE="$(cd "$(dirname "$0")" && pwd)"; ID="$(dirname "$(dirname "$HERE")")/lab/monado/input_driver"
d=${1:--0.2,1}
$ID lforce=1 >/dev/null; sleep 0.6; $ID lforce=1 rstick=$d >/dev/null; sleep 0.8; $ID rstick=$d >/dev/null; sleep 0.2; $ID >/dev/null
