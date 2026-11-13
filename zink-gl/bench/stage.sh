#!/bin/bash
# SPDX-License-Identifier: MIT
# stage.sh OUT: the B580 measurement kit of docs/design/DESIGN-zink-gpu-gap.md in OUT (on the build Mac; nothing is installed):
#   OUT/lib/  libEGL.1.dylib + libgallium-27.0.0-devel.dylib from a zink Mesa build (ZG_MESA_BUILD, with the
#             ZINK_BAR_BUFFERS patch mesa/patches/0107), libglfw.3.dylib of the package (ZG_PKG_LIB), libcglzink.dylib
#             (cgl/build.sh output ZG_CGL), the same rpaths as the package (@loader_path; gallium: /usr/local/lib)
#   OUT/lib/anv/  only with ZG_ANV=<libvulkan_intel.dylib>: that ANV build (e.g. the released source + mesa/patches/0108)
#             and an ICD manifest beside it; the kit then gives zink this ANV through VK_DRIVER_FILES (the Metal plugin
#             keeps the installed one: it opens its driver by path)
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
rm -rf "$O/lib/anv"
if [ -n "${ZG_ANV:-}" ]; then
    mkdir -p "$O/lib/anv"
    cp -f "$ZG_ANV" "$O/lib/anv/libvulkan_intel.dylib"
    # a library_path with a slash is relative to the manifest (Khronos loader)
    printf '{\n    "file_format_version": "1.0.1",\n    "ICD": {\n        "library_path": "./libvulkan_intel.dylib",\n        "library_arch": "64",\n        "api_version": "1.4.363",\n        "is_portability_driver": false\n    }\n}\n' > "$O/lib/anv/intel_icd.json"
fi
# the package's rpaths: libEGL finds libgallium next to it, libgallium the Vulkan loader in /usr/local/lib
install_name_tool -delete_rpath @loader_path/../gallium/targets/dri "$O/lib/libEGL.1.dylib" 2>/dev/null || true
install_name_tool -add_rpath @loader_path "$O/lib/libEGL.1.dylib" 2>/dev/null || true
otool -l "$O/lib/libgallium-27.0.0-devel.dylib" | grep -q "path /usr/local/lib " ||
    install_name_tool -add_rpath /usr/local/lib "$O/lib/libgallium-27.0.0-devel.dylib"
clang -arch x86_64 -mmacosx-version-min=15.0 -O2 -Wall -Wextra -Wno-unused-parameter -Wno-deprecated-declarations \
    -Wno-four-char-constants "$D/zgbench.c" -framework OpenGL -framework IOSurface -framework CoreFoundation -o "$O/bin/zgbench"
cp -f "$D/zinkgap-kit.sh" "$O/bin/" && chmod +x "$O/bin/zinkgap-kit.sh"
for f in "$O"/lib/*.dylib "$O"/lib/anv/*.dylib "$O/bin/zgbench" "$O/bin/layertest" "$O/bin/libfakeegl.dylib"; do
    if [ -e "$f" ]; then codesign -s - -f "$f" 2>/dev/null; fi; done
otool -l "$O/lib/libEGL.1.dylib" | grep -A2 LC_RPATH | grep -q "@loader_path" || { echo "stage: libEGL rpath not set"; exit 1; }
(cd "$O" && find lib bin -type f | sort | xargs shasum -a 256 > SHA256SUMS)
cat "$O/SHA256SUMS"
