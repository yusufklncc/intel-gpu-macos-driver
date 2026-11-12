#!/bin/sh
# SPDX-License-Identifier: MIT
# build.sh: libintelxevenc.dylib, the IntelXe HEVC/H.264/AV1 VideoToolbox encoders, against FFmpeg's shared libraries
# (ffmpeg/configure-shared.sh), install name /Library/GPUBundles/nvmtl/libintelxevenc.dylib; and vtenctest, vtencq.
#   FFMPEG_INSTALL=<ffmpeg install prefix> MESA_INCLUDE=<mesa>/include [OUT=dir] encoder/build.sh
# Every Mach-O is ad-hoc signed: x86_64 links made on arm64 are not, and dyld refuses unsigned code in platform and
# library-validated processes (WindowServer, VTDecoderXPCService).
set -eu
HERE=$(cd "$(dirname "$0")" && pwd)
OUT=${OUT:-$HERE/build}
: "${FFMPEG_INSTALL:?}" "${MESA_INCLUDE:?}"
SDK=${SDK:-/Library/Developer/CommandLineTools/SDKs/MacOSX26.5.sdk}
CC="xcrun --sdk $SDK clang -isysroot $SDK -arch x86_64 -mmacosx-version-min=15.0 -O2 -g -Wall -Wno-deprecated-declarations -fvisibility=hidden"
mkdir -p "$OUT"
$CC -dynamiclib -o "$OUT/libintelxevenc.dylib" "$HERE/intelxevenc.c" -I"$FFMPEG_INSTALL/include" -I"$MESA_INCLUDE" \
  -L"$FFMPEG_INSTALL/lib" -lavcodec -lavutil -install_name /Library/GPUBundles/nvmtl/libintelxevenc.dylib \
  -framework CoreFoundation -framework CoreMedia -framework CoreVideo -framework VideoToolbox -framework IOKit -framework IOSurface
$CC -fobjc-arc -o "$OUT/vtenctest" "$HERE/vtenctest.m" -framework Foundation -framework CoreMedia -framework CoreVideo \
  -framework VideoToolbox -framework Metal
$CC -fobjc-arc -o "$OUT/vtencq" "$HERE/../tests/vtencq.m" -framework Foundation -framework CoreMedia -framework CoreVideo \
  -framework VideoToolbox -framework Metal
# the debug map (source and object paths of this machine) stays in the .dSYM next to it, not in the shipped library
strip -S "$OUT/libintelxevenc.dylib"
for f in libintelxevenc.dylib vtenctest vtencq; do codesign -f -s - "$OUT/$f"; done
