#!/bin/bash
# Copy the current build-win mod into a lab tree (tools/lab/make_lab_tree.sh), keeping its options.txt.
HERE="$(cd "$(dirname "$0")" && pwd)"; ROOT="$(dirname "$(dirname "$HERE")")"; T="$1"
[ -d "$T/Bin/x64" ] || { echo "no lab tree $T"; exit 1; }
B="$ROOT/build-win"; M="$T/Bin/x64/ss4smvr"; mkdir -p "$M"
cp -f "$B/msimg32.dll" "$T/Bin/x64/msimg32.dll"
cp -f "$B/ss4smvr.dll" "$M/ss4smvr.dll"
cp -f "$B/openxr/src/loader/libopenxr_loader.dll" "$M/libopenxr_loader.dll"
F=/home/obesecatlord/LargeGames/Development/deps-src
cp -f "$F/fidelityfx-sdk-1.1.4/PrebuiltSignedDLL/amd_fidelityfx_vk.dll" "$M/amd_fidelityfx_vk.dll"
[ -f "$M/options.txt" ] || printf 'lab_giveall=1\n' > "$M/options.txt"
