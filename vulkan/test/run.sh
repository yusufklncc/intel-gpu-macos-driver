#!/bin/sh
# SPDX-License-Identifier: MIT
# run.sh OUTDIR: the provider's offline test (build.sh OUTDIR first). No GPU: a fake ICD stands in for ANV. The
# provider is opened by the names Wine uses, through DYLD_LIBRARY_PATH, as intelxe-vk run does it.
set -eu
O=$(cd "${1:?usage: run.sh OUTDIR}" && pwd)
P=$O/test/provider; rm -rf "$P"; mkdir -p "$P"
ln -s ../../lib/libintelxe-vk.dylib "$P/libMoltenVK.dylib"
ln -s ../../lib/libintelxe-vk.dylib "$P/libvulkan.1.dylib"
for name in libMoltenVK.dylib libvulkan.1.dylib; do
    echo "== $name"
    DYLD_LIBRARY_PATH=$P INTELXE_VK_ICD=$O/test/libfakeicd.dylib INTELXE_VK_DEBUG=1 "$O/test/provtest" $name "$O/test/libfakeicd.dylib"
done
echo "== INTELXE_VK_ICD naming a file that DYLD_LIBRARY_PATH turns into the provider"
if DYLD_LIBRARY_PATH=$P INTELXE_VK_ICD=/somewhere/libMoltenVK.dylib "$O/test/provtest" libvulkan.1.dylib "$O/test/libfakeicd.dylib" 2>&1 |
        grep -q 'is the provider itself'; then echo "ok    refused with a message (no recursion)"; else echo "FAIL  no message"; exit 1; fi
echo "== libvulkan.1.dylib with INTELXE_VK_FRAMELOG"
L=$O/test/frames-%p.txt; rm -f "$O"/test/frames-*.txt
DYLD_LIBRARY_PATH=$P INTELXE_VK_ICD=$O/test/libfakeicd.dylib INTELXE_VK_FRAMELOG=$L "$O/test/provtest" libvulkan.1.dylib "$O/test/libfakeicd.dylib"
F=$(ls "$O"/test/frames-*.txt 2>/dev/null | head -1)
if [ -n "$F" ] && [ "$(grep -c '^[0-9]' "$F")" = 5 ] && grep -q '^# end: 5 presents' "$F"; then
    echo "ok    frame log $(basename "$F"): 5 lines and the end line (%p = the pid)"
else
    echo "FAIL  frame log: ${F:-none written}"; [ -n "$F" ] && cat "$F"; exit 1
fi
D=$(cd "$(dirname "$0")/.." && pwd)
"$D/intelxe-vk" frametimes "$F" | sed 's/^/      /'
"$D/intelxe-vk" frametimes "$F" | grep -q '^summary frames=5 .* intervals=4 fps=[1-9]' || { echo "FAIL  intelxe-vk frametimes"; exit 1; }
echo "ok    intelxe-vk frametimes reads it"
