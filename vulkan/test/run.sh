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
