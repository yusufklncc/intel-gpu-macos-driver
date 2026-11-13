#!/bin/bash
# SPDX-License-Identifier: MIT
# build.sh OUTDIR: the native Vulkan provider in the package layout, x86_64, ad-hoc signed. Builds on the build Mac.
#   OUTDIR/lib/libintelxe-vk.dylib   the provider (src/intelxe_vk.m); install.sh links it as libMoltenVK.dylib and
#                                    libvulkan.1.dylib in /Library/IntelXe/Vulkan/provider
#   OUTDIR/bin/vksurf                the swapchain test (user/tests/vulkan/vksurf.m)
#   OUTDIR/bin/vkscene               a renderer-path sample: MSAA, depth, bindless textures, timeline semaphores,
#                                    timings and a read-back checksum (user/tests/vulkan/vkscene.m)
#   OUTDIR/icd.d/intel_icd.x86_64.json  ANV's manifest for the Khronos loader (user/vulkan-loader)
#   OUTDIR/test/libfakeicd.dylib, OUTDIR/test/provtest   the provider's offline test (test/run.sh; runs on any Mac,
#                                    under Rosetta on Apple silicon; no GPU needed)
# Vulkan headers: VK_HEADERS (a folder with vulkan/vulkan.h, Vulkan-Headers 1.4.x), by default the Mesa tree's.
set -euo pipefail
O=${1:?usage: build.sh OUTDIR}
D=$(cd "$(dirname "$0")" && pwd)
VKH=${VK_HEADERS:-$HOME/hackintosh/work/mesa014/include}
[ -f "$VKH/vulkan/vulkan_metal.h" ] || { echo "no Vulkan headers in $VKH (set VK_HEADERS)"; exit 1; }
mkdir -p "$O/lib" "$O/bin" "$O/test" "$O/icd.d"
cp "$D/../user/vulkan-loader/intel_icd.x86_64.json" "$O/icd.d/"
F=(-arch x86_64 -mmacosx-version-min=15.0 -O2 -Wall -Wno-deprecated-declarations -I"$VKH")
# -fvisibility=hidden: only the names marked EXPORT leave the library (Wine dlsym()s a few; nothing else may clash)
clang "${F[@]}" -fvisibility=hidden -fobjc-arc -dynamiclib "$D/src/intelxe_vk.m" -framework AppKit -framework QuartzCore \
    -install_name /Library/IntelXe/Vulkan/lib/libintelxe-vk.dylib -current_version 1 -compatibility_version 1 \
    -o "$O/lib/libintelxe-vk.dylib"
clang "${F[@]}" -fobjc-arc "$D/../user/tests/vulkan/vksurf.m" -framework AppKit -framework QuartzCore -framework Metal \
    -framework IOSurface -o "$O/bin/vksurf"
clang "${F[@]}" -fobjc-arc "$D/../user/tests/vulkan/vkscene.m" -framework AppKit -framework QuartzCore -framework Metal \
    -framework IOSurface -framework ImageIO -framework UniformTypeIdentifiers -o "$O/bin/vkscene"
clang "${F[@]}" -dynamiclib "$D/test/fakeicd.c" -o "$O/test/libfakeicd.dylib"
clang "${F[@]}" -fobjc-arc "$D/test/provtest.m" -framework AppKit -framework QuartzCore -o "$O/test/provtest"
for f in "$O/lib/libintelxe-vk.dylib" "$O/bin/vksurf" "$O/bin/vkscene" "$O/test/libfakeicd.dylib" "$O/test/provtest"; do codesign -s - -f "$f" 2>/dev/null; done
lipo -archs "$O/lib/libintelxe-vk.dylib"
