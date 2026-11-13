#!/bin/sh
# SPDX-License-Identifier: MIT
# configure-shared.sh: FFmpeg 9.0.2 with the patches in this directory, as the shared libavcodec/libavutil the IntelXe VideoToolbox decoder and
# encoders load from /Library/GPUBundles/nvmtl (install names point there). LGPL-2.1-or-later only: no --enable-gpl,
# --enable-version3 or --enable-nonfree. The AV1, H.264, HEVC and VP9 decoders with their Vulkan hwaccels, and the Vulkan encoders.
#   FFMPEG_SRC=<ffmpeg-9.0.2 source> MESA_INCLUDE=<mesa>/include [PREFIX=<install prefix>] configure-shared.sh
#   then: make -j8 && make install   (in the directory this was run from)
# The source: ffmpeg-9.0.2.tar.xz with apply-patches.sh (the eight patches of the released libraries, in order, then the later ones). The
# configure line is recorded in libavcodec (avcodec_configuration()); the release libraries were configured from the
# build directory with relative paths, PREFIX=install MESA_INCLUDE=vk-include (a copy of <mesa>/include there).
set -eu
: "${FFMPEG_SRC:?}" "${MESA_INCLUDE:?}"
PREFIX=${PREFIX:-$PWD/install}
"$FFMPEG_SRC/configure" --prefix="$PREFIX" --install-name-dir=/Library/GPUBundles/nvmtl \
  --arch=x86_64 --target-os=darwin --enable-cross-compile \
  --cc='xcrun clang -arch x86_64 -mmacosx-version-min=15.0' --objcc='xcrun clang -arch x86_64 -mmacosx-version-min=15.0' \
  --enable-shared --disable-static --disable-x86asm --disable-autodetect --disable-everything --disable-programs \
  --disable-doc --disable-network --disable-avformat --disable-avdevice --disable-avfilter --disable-swscale \
  --disable-swresample --enable-vulkan --enable-decoder='av1,h264,hevc,vp9' --enable-hwaccel='av1_vulkan,h264_vulkan,hevc_vulkan,vp9_vulkan' \
  --enable-encoder='av1_vulkan,h264_vulkan,hevc_vulkan' --enable-pic --extra-cflags="-I$MESA_INCLUDE"
