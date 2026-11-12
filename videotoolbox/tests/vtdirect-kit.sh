#!/bin/sh
# SPDX-License-Identifier: MIT
# vtdirect-kit.sh <out> <conformance dir>: the folder the night A/B (vtdirect-ab.sh) runs from on the B580 machine:
# the decoder test build and vtdec (build.sh), vtdirect-ab.sh, and conf/: the JVT H.264 conformance streams of
# <conformance dir> (tests/h264/run.sh --conformance downloads them) remuxed into MP4 (ffmpeg -c copy), those the
# direct path covers (its parser agrees with libavcodec on them here) and three it hands to libavcodec at the start
# (fields, MBAFF, frame_num gaps). SHA256SUMS last. Copy the folder to the machine as it is.
#   FFMPEG_INSTALL=... MESA_INCLUDE=... vtdirect-kit.sh <out> <conformance dir> [ffmpeg]
set -eu
HERE=$(cd "$(dirname "$0")" && pwd)
OUT=${1:?usage: vtdirect-kit.sh <out> <conformance dir> [ffmpeg]}; CONF=${2:?conformance dir}; FF=${3:-ffmpeg}
mkdir -p "$OUT/conf"
OUT=$(cd "$OUT" && pwd)
OUT="$OUT" sh "$HERE/../build.sh"
rm -f "$OUT/libavcodec.63.dylib" "$OUT/libavutil.61.dylib"   # the installed ones are used (this change does not touch FFmpeg)
cp "$HERE/vtdirect-ab.sh" "$OUT/"
T=$(mktemp -d); trap 'rm -rf "$T"' EXIT
cc -O1 -I"$MESA_INCLUDE" "$HERE/h264parse_test.c" "$HERE/../vtdirect_h264.c" -o "$T/p"
n=0
for f in "$CONF"/*; do
  b=$(basename "$f"); b=${b%.*}
  if "$T/p" "$f" > /dev/null 2>&1; then :; else
    case "$b" in
      CVFI1_SVA_C|CAMA1_TOSHIBA_B|MR4_TANDBERG_C) ;;   # not covered: libavcodec from StartSession on
      *) continue;;
    esac
  fi
  "$FF" -hide_banner -v error -i "$f" -c copy -y "$OUT/conf/$b.mp4" && n=$((n + 1))
done
( cd "$OUT" && find . -type f ! -name SHA256SUMS | sort | xargs shasum -a 256 > SHA256SUMS )
echo "kit: $OUT ($n conformance streams)"
