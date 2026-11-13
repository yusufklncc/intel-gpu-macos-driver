#!/bin/bash
# SPDX-License-Identifier: MIT
# install.sh (vulkan): native Vulkan for games, EXPERIMENTAL. Puts the Vulkan provider (libintelxe-vk: ANV for Wine's
# macOS Vulkan driver and other programs that open MoltenVK or libvulkan.1.dylib by name) into /Library/IntelXe/Vulkan.
# Nothing loads it unless started through ./intelxe-vk run (DYLD_LIBRARY_PATH for that process only); Wine's MoltenVK,
# the Khronos loader in /usr/local/lib, ANV and the Metal driver are not touched.
#
#   sudo ./install.sh          install (backs up what it replaces to ./previous/, once)
#   sudo ./install.sh --undo   put back what was there before
#   ./install.sh --status      what is installed
#   ./install.sh --test        vksurf through the provider: a window, 120 frames, PASS/FAIL (~3 s)
#
# Installs /Library/IntelXe/Vulkan/:
#   lib/libintelxe-vk.dylib              the provider
#   provider/libMoltenVK.dylib, provider/libvulkan.1.dylib   links to it: the folder "intelxe-vk run" puts first in
#                                        DYLD_LIBRARY_PATH, so either name a Wine build opens gets ANV
#   loader/libMoltenVK.dylib             a link to the Khronos loader (/usr/local/lib/libvulkan.1.dylib, from zink-gl),
#                                        for "intelxe-vk run --loader" (Wine builds that open MoltenVK by name, with the
#                                        loader's layers available)
#   icd.d/intel_icd.x86_64.json          the loader's manifest for ANV (VK_DRIVER_FILES)
#   bin/intelxe-vk, bin/vksurf, bin/vkscene   the helper, the swapchain test and the renderer-path sample
# Not in /Library/GPUBundles/nvmtl: the driver's install.sh replaces that folder whole on every --apply and --undo.
set -euo pipefail
H=$(cd "$(dirname "$0")" && pwd)
B=$H/previous
VK=/Library/IntelXe/Vulkan
ANV=/Library/GPUBundles/nvmtl/libvulkan_intel.dylib
LOADER=/usr/local/lib/libvulkan.1.dylib

root() { [ "$(id -u)" = 0 ] || { echo "needs root: sudo $0 $*"; exit 1; }; }
sums() { (cd "$H" && shasum -a 256 -c SHA256SUMS >/dev/null) || { echo "REFUSED: files differ from SHA256SUMS"; exit 1; }; }
sig() { "$H/sigcheck" "$1" >/dev/null || { "$H/sigcheck" "$1"; echo "REFUSED: $1 is not signed"; exit 1; }; }

status() {
    echo "native Vulkan (experimental):"
    if [ -f $VK/lib/libintelxe-vk.dylib ]; then
        echo "  provider: $VK/lib/libintelxe-vk.dylib ($(grep -a -o -m1 'intelxe-vk-provider [0-9]*' $VK/lib/libintelxe-vk.dylib || echo '?'))"
    else echo "  provider: not installed"; fi
    [ -f $ANV ] && echo "  ANV: $ANV" || echo "  ANV: MISSING (install the driver)"
    if [ -e $LOADER ]; then echo "  Khronos loader: $LOADER (intelxe-vk run --loader works)"
    else echo "  Khronos loader: none in /usr/local/lib (only intelxe-vk run --loader needs it: install zink-gl)"; fi
    echo "  helper: $VK/bin/intelxe-vk check   (Wine builds and prefixes)"
}

# place SRC DST: replace DST with a root-owned copy of SRC
place() { rm -rf "$2.new"; ditto "$1" "$2.new"; chown -R root:wheel "$2.new"; chmod -R go-w "$2.new"; rm -rf "$2"; mv "$2.new" "$2"; }

install_all() {
    root
    sums
    [ -f $ANV ] || { echo "REFUSED: the driver is not installed (no $ANV)"; exit 1; }
    sig "$H/lib/libintelxe-vk.dylib"; sig "$H/bin/vksurf"; sig "$H/bin/vkscene"
    if [ ! -d "$B" ]; then
        mkdir -p "$B"
        if [ -d $VK ]; then ditto $VK "$B/Vulkan"; else touch "$B/no-Vulkan"; fi
        echo "backup: $B"
    fi
    local S=$H/.stage/Vulkan
    rm -rf "$H/.stage"; mkdir -p "$S/lib" "$S/provider" "$S/loader" "$S/icd.d" "$S/bin"
    install -m 755 "$H/lib/libintelxe-vk.dylib" "$S/lib/"
    ln -s ../lib/libintelxe-vk.dylib "$S/provider/libMoltenVK.dylib"
    ln -s ../lib/libintelxe-vk.dylib "$S/provider/libvulkan.1.dylib"
    ln -s $LOADER "$S/loader/libMoltenVK.dylib"   # dangling until a loader is there; intelxe-vk run --loader checks
    install -m 644 "$H/icd.d/intel_icd.x86_64.json" "$S/icd.d/"
    install -m 755 "$H/intelxe-vk" "$H/bin/vksurf" "$H/bin/vkscene" "$S/bin/"
    mkdir -p /Library/IntelXe
    place "$S" $VK; rm -rf "$H/.stage"
    status
    echo "installed. Try: $0 --test; then $VK/bin/intelxe-vk check"
}

undo() {
    root
    [ -d "$B" ] || { echo "nothing to undo (not installed from this folder)"; exit 1; }
    if [ -d "$B/Vulkan" ]; then place "$B/Vulkan" $VK; else rm -rf $VK; rmdir /Library/IntelXe 2>/dev/null || true; fi
    rm -rf "$B"
    echo "undone. Wine prefixes keep their DXVK/vkd3d-proton DLLs: intelxe-vk prefix-restore PREFIX puts theirs back."
    status
}

selftest() {
    [ -x $VK/bin/vksurf ] || { echo "install first: sudo $0"; exit 1; }
    # perl sets the variable itself: one handed to /usr/bin/perl could be dropped at its start (a protected binary)
    exec /usr/bin/perl -e '$ENV{DYLD_LIBRARY_PATH} = shift; alarm shift; exec @ARGV' $VK/provider 30 \
        $VK/bin/vksurf --lib libMoltenVK.dylib --wine10 --frames 120
}

case "${1:-}" in
    "") install_all ;;
    --undo) undo ;;
    --status) status ;;
    --test) selftest ;;
    *) sed -n '3,12p' "$0"; exit 2 ;;
esac
