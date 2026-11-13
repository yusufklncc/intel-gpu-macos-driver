#!/bin/bash
# SPDX-License-Identifier: MIT
# stage.sh OUT: the B580 measurement kit of docs/design/DESIGN-zink-gpu-gap.md in OUT (on the build Mac; nothing is installed):
#   OUT/lib/  libEGL.1.dylib + libgallium-27.0.0-devel.dylib from a zink Mesa build (ZG_MESA_BUILD, with the
#             ZINK_BAR_BUFFERS patch mesa/patches/0107), libglfw.3.dylib of the package (ZG_PKG_LIB), libcglzink.dylib
#             (cgl/build.sh output ZG_CGL), the same rpaths as the package (@loader_path; gallium: /usr/local/lib)
#   OUT/bin/  zgbench (x86_64), layertest + libfakeegl.dylib (cgl/test), zinkgap-kit.sh
#   OUT/SHA256SUMS
# Every binary is ad-hoc signed, as the package's are. The kit loads only OUT/lib (zgbench dlopens OUT/lib's GLFW,
# which loads the libEGL next to it, which loads the libgallium next to it), so no DYLD_* variable is needed for zink.
set -euo pipefail
O=${1:?usage: stage.sh OUTDIR}
D=$(cd "$(dirname "$0")" && pwd)
MB=${ZG_MESA_BUILD:-$HOME/hackintosh/work/zinkgap-mesa/build}
PKG=${ZG_PKG_LIB:-$HOME/hackintosh/work/zink014/lib}
CGL=${ZG_CGL:-$HOME/hackintosh/work/zinkgap-x86}
mkdir -p "$O/lib" "$O/bin"
cp -f "$MB/src/egl/libEGL.1.dylib" "$MB/src/gallium/targets/dri/libgallium-27.0.0-devel.dylib" "$O/lib/"
cp -f "$PKG/libglfw.3.dylib" "$O/lib/"
cp -f "$CGL/libcglzink.dylib" "$O/lib/"
cp -f "$CGL/layertest" "$CGL/libfakeegl.dylib" "$O/bin/"
# the package's rpaths: libEGL finds libgallium next to it, libgallium the Vulkan loader in /usr/local/lib
install_name_tool -delete_rpath @loader_path/../gallium/targets/dri "$O/lib/libEGL.1.dylib" 2>/dev/null || true
install_name_tool -add_rpath @loader_path "$O/lib/libEGL.1.dylib" 2>/dev/null || true
otool -l "$O/lib/libgallium-27.0.0-devel.dylib" | grep -q "path /usr/local/lib " ||
    install_name_tool -add_rpath /usr/local/lib "$O/lib/libgallium-27.0.0-devel.dylib"
clang -arch x86_64 -mmacosx-version-min=15.0 -O2 -Wall -Wextra -Wno-unused-parameter -Wno-deprecated-declarations \
    -Wno-four-char-constants "$D/zgbench.c" -framework OpenGL -framework IOSurface -framework CoreFoundation -o "$O/bin/zgbench"
cp -f "$D/zinkgap-kit.sh" "$O/bin/" && chmod +x "$O/bin/zinkgap-kit.sh"
for f in "$O"/lib/*.dylib "$O/bin/zgbench" "$O/bin/layertest" "$O/bin/libfakeegl.dylib"; do codesign -s - -f "$f" 2>/dev/null; done
otool -l "$O/lib/libEGL.1.dylib" | grep -A2 LC_RPATH | grep -q "@loader_path" || { echo "stage: libEGL rpath not set"; exit 1; }
(cd "$O" && shasum -a 256 lib/* bin/* > SHA256SUMS)
cat "$O/SHA256SUMS"
